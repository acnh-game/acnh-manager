#include "app.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>

#include "manifest/manifest.hpp"
#include "install/content_install.hpp"
#include "install/container.hpp"
#include "install/game_install.hpp"
#include "install/source_local_file.hpp"
#include "install/network_source.hpp"
#include "log.hpp"
#include "net/update_policy.hpp"
#include "version.hpp"
#include "probe.hpp"
#include "util/fs_path.hpp"

namespace acnh_manager::ui {

void App::Trace(const char *fmt, ...) {
    if (m_log == nullptr) {
        return;
    }
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log->Line("%s", buf);
    m_log->Sync();
}

namespace {

/* Development switches live as empty files on the card; the app only checks whether the file
   can be opened, never its contents. */
bool DevFlagPresent(FsFileSystem &sd, const char *path) {
    FsFile flag{};
    const util::FsPath arg(path);
    if (R_FAILED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &flag))) {
        return false;
    }
    fsFileClose(&flag);
    return true;
}

/* Family palette (same origin as the icon): teal accent, cream background, sand highlights. */
constexpr Color kBackground{0xF6, 0xF1, 0xE3, 0xFF};
constexpr Color kHeader{0x2F, 0xBF, 0xA8, 0xFF};
constexpr Color kCard{0xFF, 0xFD, 0xF7, 0xFF};
constexpr Color kBorder{0xD8, 0xCF, 0xB6, 0xFF};
constexpr Color kText{0x2B, 0x2B, 0x2B, 0xFF};
constexpr Color kSubtle{0x6B, 0x6B, 0x6B, 0xFF};
constexpr Color kGood{0x1B, 0x7F, 0x5A, 0xFF};
/* Warnings are amber, not red.  The old value sat at a hue of 24 degrees, which read as brick red
   on the console; kBad is the palette's only red, and it means "the action failed".  Amber is a
   hue near 40 degrees: kWarn carries text and the status dot, kWarnBright is the strip's border
   and mark -- decoration, so it can be lighter than a legible text colour. */
constexpr Color kWarn{0x9A, 0x62, 0x00, 0xFF};
constexpr Color kWarnBright{0xD2, 0x9A, 0x00, 0xFF};
/* Fill behind the home warning strip: pale yellow rather than peach, so the whole strip reads as
   a caution instead of a pinkish alert. */
constexpr Color kWarnTint{0xFB, 0xF1, 0xCE, 0xFF};
constexpr Color kOnHeader{0xFF, 0xFF, 0xFF, 0xFF};
constexpr Color kHeaderSubtle{0xD5, 0xF2, 0xEC, 0xFF};
constexpr Color kHeaderBadge{0x59, 0xCD, 0xB9, 0xFF};
constexpr Color kBad{0xC0, 0x39, 0x2B, 0xFF};
constexpr Color kCardDisabled{0xF0, 0xEC, 0xE0, 0xFF};
/* State colour for "nothing installed yet": purple, so it reads differently from the teal of
   the action button and from the green/orange/red of the other states. */
constexpr Color kUninstalled{0x8A, 0x62, 0xD4, 0xFF};

/* Taller than a plain title bar because the header carries a subtitle: what this program is,
   for the people who just installed it. */
constexpr int kHeaderHeight = 96;
constexpr int kFooterHeight = 44;
constexpr int kMargin = 40;
constexpr int kCardGap = 18;
/* Card layout: header height, horizontal padding, distance from the body to the bottom
   border, label column width, row gap.  Card height is measured from the content (see
   RowsHeight); when a page does not fit, the row gap tightens first and clipping is the
   last resort. */
/* Title band.  It was 56 px; 48 frees 8 px per card, which is what lets the details page keep
   a comfortable bottom padding in English (where one row wraps to two lines). */
constexpr int kCardHeader = 48;
constexpr int kCardPadX = 24;
/* Breathing room under the last row: 12 px read as "the text is falling out of the card"
   once a card had no row gap left. */
constexpr int kCardPadBottom = 22;
/* Top inset used when a card has no title (see DrawCard). */
constexpr int kCardPadTop = 26;
/* Gap between the file list's card title and its first row. */
constexpr int kListTopGap = 14;
/* The label column has to fit the longest English label ("Existing override files", about
   255 px), otherwise it would overlap the value. */
constexpr int kLabelColumn = 280;
constexpr int kRowGap = 8;

/* Home-screen action ids: the three controls of the new one-screen UI. */
constexpr int kActionPrimary = 0;     /* install / update / repair / retry / check again (A) */
constexpr int kActionCheckUpdate = 1; /* X */
constexpr int kActionUninstall = 2;   /* Y */
constexpr int kActionBack = 3;        /* B: leave the confirmation pages */
constexpr int kActionTabStatus = 10;  /* L: the status page (also a touch target) */
constexpr int kActionTabDetails = 11; /* R: the details page (also a touch target) */
constexpr int kActionFooterBack = 12; /* the footer's "Ⓑ exit / back" hint */
constexpr int kActionLanguage = 13;   /* the details page's language row */
constexpr int kActionGuide = 14;      /* the header's "-" entry: the chat-code explainer */
constexpr int kActionGameInstall = 15; /* the header's "+" entry: the game installer (modal page) */
constexpr int kActionInstallLocal = 16;    /* the installer's "this console" card (Y) */
constexpr int kActionInstallDownload = 17; /* the installer's "download" card (X) */
constexpr int kActionInstallUpload = 18;   /* the installer's "upload" card (A) */
constexpr int kActionInstallPickRow = 19; /* a row of the installer's file list */
constexpr int kActionInstallStart = 20;   /* "start" on the confirm screen (A) */

/* Player settings (language today).  Separate from state.json because uninstall deletes that. */
constexpr const char *kSettingsPath = "/switch/ACNH-Manager/settings.json";

/* Header tab layout lives in ui/header_tabs.hpp so the host tests can pin it; these two
   constants have to agree with it. */
static_assert(kMargin == ui::kHeaderMargin, "header tab layout assumes the page margin");
static_assert(kHeaderHeight == ui::kHeaderBarHeight, "header tab layout assumes the bar height");

/* Footer hint geometry: the drawn "Ⓑ exit / back" badge plus its label and the touch target
   share these numbers, so the hint is a real button on a touch screen. */
struct FooterHintLayout {
    int badge_x;
    int badge_y;
    int badge;
    int text_y;
    int label_x;
    Rect hit;
};

FooterHintLayout LayoutFooterHint(int surface_height, int line_height, int label_width) {
    FooterHintLayout layout{};
    const int y = surface_height - kFooterHeight;
    layout.badge = 30;
    layout.badge_x = kMargin;
    layout.badge_y = y + (kFooterHeight - layout.badge) / 2;
    layout.text_y = y + (kFooterHeight - line_height) / 2 + 4;
    layout.label_x = kMargin + layout.badge + 12;
    const int right = label_width > 0 ? layout.label_x + label_width
                                      : layout.badge_x + layout.badge;
    layout.hit = Rect{layout.badge_x - 8, y, right - layout.badge_x + 16, kFooterHeight};
    return layout;
}

/* Badge colour used on the confirmation buttons (the home screen's accent teal). */
inline Color dot_face() { return kHeader; }

/* Sizes for a human: the file list is the one place we show them, and "73986 B" is noise.
   Two significant digits are plenty when the exact value lives in state.json. */
std::string FormatSize(u64 bytes) {
    char buffer[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buffer;
}


const char *PlanActionName(install::PlanAction action) {
    switch (action) {
        case install::PlanAction::Blocked: return "blocked";
        case install::PlanAction::Install: return "install";
        case install::PlanAction::Repair: return "repair";
        case install::PlanAction::UpToDate: return "up-to-date";
    }
    return "?";
}


}  // namespace

bool App::Init(acnh_manager::Log *log, FsFileSystem &sd, std::string *error) {
    m_log = log;
    m_sd = &sd;
    m_update_task.SetLog(log);
    /* The chat-code explainer reads its code once; empty when the build carries none. */
    m_guide_qr = ui::QrMatrix::FromEmbedded();
    LoadSettings();
    Trace("ui: font init ...");
    if (!m_font.Init(log, error)) {
        return false;
    }
    Trace("ui: font init ok");
    /* Same as EdiZon-SE: always ask for two buffers. */
    const u32 num_fbs = 2;
    Trace("ui: framebufferCreate ... (applet=%d num_fbs=%u)",
          static_cast<int>(appletGetAppletType()), num_fbs);
    const Result rc_create =
        framebufferCreate(&m_fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, num_fbs);
    if (R_FAILED(rc_create)) {
        Trace("ui: framebufferCreate rc=0x%08X", rc_create);
        if (error != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "framebufferCreate rc=0x%08X", rc_create);
            *error = buf;
        }
        m_font.Exit();
        return false;
    }
    Trace("ui: framebufferCreate ok");
    Trace("ui: framebufferMakeLinear ...");
    const Result rc_linear = framebufferMakeLinear(&m_fb);
    if (R_FAILED(rc_linear)) {
        Trace("ui: framebufferMakeLinear rc=0x%08X", rc_linear);
        if (error != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "framebufferMakeLinear rc=0x%08X", rc_linear);
            *error = buf;
        }
        framebufferClose(&m_fb);
        m_font.Exit();
        return false;
    }
    Trace("ui: framebufferMakeLinear ok (buf=%p linear=%p)", m_fb.buf, m_fb.buf_linear);
    m_fb_ready = true;
    Trace("ui: collecting environment ...");
    Collect();
    /* One silent look at the release host per launch: a newer agent shows up on the home screen
       on its own, and a failure only lands in the log/details. */
    StartUpdateCheck(/*silent=*/true);
    Trace("ui: environment collected (exefs=%zu, manifest=%d)", m_report.exefs.size(),
          m_have_manifest ? 1 : 0);
    /* Crash leftovers: an interrupted two-phase install can leave `<target>.acnh-tmp` /
       `<target>.acnh-old` siblings behind.  Clean them at startup so the directory never
       accumulates junk, and say so in the log. */
    if (m_sd != nullptr) {
        /* The manifest is the only source for where our files live; without it there is
           nothing we could have written, so there is nothing to clean either. */
        std::string dir;
        if (m_plan.game != nullptr && !m_plan.game->files.empty()) {
            const std::string &target = m_plan.game->files.front().target;
            const std::size_t slash = target.find_last_of('/');
            if (slash != std::string::npos) {
                dir = target.substr(0, slash);
            }
        }
        if (!dir.empty()) {
            std::vector<std::string> leftovers;
            const int cleaned = install::CleanLeftovers(*m_sd, dir, &leftovers);
            /* Log whenever anything was *found*, including leftovers that refused to be removed:
               a stuck temp file is the difference between a working and a failing install, and
               this line is where it shows up. */
            if (!leftovers.empty() && m_log != nullptr) {
                std::string list;
                for (const auto &name : leftovers) {
                    if (!list.empty()) {
                        list += ", ";
                    }
                    list += name;
                }
                m_log->Line("cleanup: removed %d leftover file(s): %s", cleaned, list.c_str());
            }
        }
    }
    /* Debug switch: while /switch/ACNH-Manager/dev-pause exists, stop before the first frame
       and wait for +, so sys-agent can read the memory layout of a live process (see
       docs/device-acceptance.md). */
    {
        FsFile flag{};
        /* Even a literal path goes through util::FsPath: fs declares FS_MAX_PATH bytes for it,
           and the rule is easier to keep than to remember the exceptions to. */
        const util::FsPath pause_flag("/switch/ACNH-Manager/dev-pause");
        const bool pause_before_first_frame =
            m_sd != nullptr &&
            R_SUCCEEDED(fsFsOpenFile(m_sd, pause_flag.c_str(), FsOpenMode_Read, &flag));
        if (pause_before_first_frame) {
            fsFileClose(&flag);
            Trace("ui: paused before first frame (press + to continue)");
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
                    break;
                }
            }
            Trace("ui: resuming, drawing first frame");
            m_stage_pause = true;
        }
    }
    /* Paint one frame right away: that separates "the framebuffer works" from "collecting
       data works". */
    Trace("ui: first frame ...");
    Render();
    Trace("ui: first frame done");
    return true;
}

void App::Exit() {
    m_update_task.Stop();
    if (m_fb_ready) {
        framebufferClose(&m_fb);
        m_fb_ready = false;
    }
    m_font.Exit();
}

void App::Collect() {
    if (m_sd == nullptr) {
        return;
    }
    m_report = env::Collect(*m_sd);

    m_have_state = false;
    m_state_error.clear();
    if (!install::ReadStateFile(*m_sd, &m_state, &m_have_state, &m_state_error)) {
        m_state_error = i18n::Format(i18n::StringId::StateParseFailed, m_state_error.c_str());
    }

    m_have_manifest = false;
    m_manifest_embedded = false;
    m_manifest_error.clear();
    /* Official channel: the release manifest embedded in the NRO (works offline). */
    if (payload::EmbeddedAvailable()) {
        const auto parsed = manifest::Parse(payload::EmbeddedManifestJson(), kAppVersion);
        if (parsed.ok) {
            m_manifest = parsed.manifest;
            m_have_manifest = true;
            m_manifest_embedded = true;
        } else {
            m_manifest_error =
                i18n::Format(i18n::StringId::ManifestEmbeddedInvalid, parsed.error.c_str());
        }
    }
    /* Development channel: the SD manifest is only read when the official channel is
       unavailable and the settings page explicitly allows dev manifests. */
    if (!m_have_manifest) {
        bool found = false;
        std::string dev_error;
        if (!install::ReadManifestFile(*m_sd, m_dev_manifest_path, kAppVersion, !m_allow_dev_manifest,
                                       &m_manifest, &found, &dev_error)) {
            dev_error = i18n::Format(i18n::StringId::ManifestInvalid, dev_error.c_str());
        }
        if (found) {
            m_have_manifest = true;
        } else if (!dev_error.empty()) {
            m_manifest_error =
                m_manifest_error.empty() ? dev_error : m_manifest_error + "; " + dev_error;
        }
    }
    /* The UI only has room for a short hint: the full diagnostic (including the raw manifest
       validation text) always lands in log.txt. */
    if (!m_state_error.empty()) {
        Trace("collect: state error: %s", m_state_error.c_str());
    }
    if (!m_manifest_error.empty()) {
        Trace("collect: manifest error: %s", m_manifest_error.c_str());
    }
    if (!m_report.problems.empty()) {
        Trace("collect: problems: %s", m_report.problems.c_str());
    }
    RefreshPlan();
    UpdateHomeState();
    Trace("collect: gate=%s plan=%s", m_gate.reason.c_str(), PlanActionName(m_plan.action));
}

/* "0.11.0(c47d2b47)": the version alone cannot tell two builds of the same version apart, so
   the first bytes of the payload hash ride along.  Same hash the installer verifies. */
std::string App::BuildLabel(const std::string &version, const std::string &sha256) const {
    std::string out = version;
    if (!sha256.empty()) {
        out += "(" + sha256.substr(0, 8) + ")";
    }
    return out;
}

/* The installed game's version in the words the console itself uses.  The NACP display string
   comes first: it is what the home menu prints under "Ver.", and for a bare 1.0.0 copy it is the
   only human-readable version there is (that build's numeric title version is 0). */
std::string App::InstalledGameVersion() const {
    if (!m_report.game_display_version.empty()) {
        return m_report.game_display_version;
    }
    /* When the detected build matched a manifest entry, that entry describes the same build, so
       its display version is a fair fallback if the NACP could not be read. */
    if (m_gate.game != nullptr && !m_gate.game->display_version.empty()) {
        return m_gate.game->display_version;
    }
    if (m_report.build.version != 0) {
        return std::to_string(m_report.build.version);
    }
    return std::string(Tr(i18n::StringId::ValueNotAvailable));
}

/* What this build of the app can install, but only when it is a *different* version from the one
   on the console.  The gate keeps the manifest entry even when it refused, so the "not supported"
   wording can name a version instead of only saying "no".  Empty means "do not name a version
   here": either nothing lists this title, or the versions agree and the gate refused for another
   reason (a content id or build id mismatch) -- printing the same number on both sides would read
   like a contradiction. */
std::string App::SupportedGameVersionNote() const {
    const manifest::GameEntry *entry =
        m_gate.game != nullptr ? m_gate.game : m_gate.title_entry;
    if (entry == nullptr) {
        return std::string();
    }
    /* The gate compares numeric title versions, people read display strings: only name a version
       when both of those say the two sides differ, so the note can never contradict the headline
       (a content id or build id refusal happens at the same version, and a stale NACP would show
       the same string on both sides). */
    if (entry->version == m_report.build.version) {
        return std::string();
    }
    const std::string supported = !entry->display_version.empty()
                                      ? entry->display_version
                                      : std::to_string(entry->version);
    return supported == InstalledGameVersion() ? std::string() : supported;
}

/* Settings live in their own file because the install record is deleted by uninstall; the
   language has to outlive that.  A missing or unreadable file is not an error: the defaults
   stand and the reason goes to the log. */
void App::LoadSettings() {
    Settings settings;
    std::string text;
    bool found = false;
    std::string error;
    if (!install::ReadTextFile(*m_sd, kSettingsPath, &text, &found, &error)) {
        Trace("settings: read failed: %s", error.c_str());
        return;
    }
    if (!found) {
        Trace("settings: no usable file (%s), keeping defaults",
              error.empty() ? "no reason given" : error.c_str());
    } else {
        std::string problem;
        if (!ParseSettings(text, &settings, &problem)) {
            Trace("settings: unusable (%s), keeping defaults", problem.c_str());
        } else {
            /* A file we could read but whose values we did not understand reports that here
               (ParseSettings keeps the value it had) -- the log should say why. */
            if (!problem.empty()) {
                Trace("settings: %s", problem.c_str());
            }
            Trace("settings: language=%s",
                  settings.language == i18n::Language::English ? "en" : "zh-Hans");
        }
    }
    m_language = settings.language;
    i18n::SetLanguage(m_language);
}

void App::SaveSettings() {
    if (m_sd == nullptr) {
        return;
    }
    Settings settings;
    settings.language = m_language;
    std::string error;
    if (!install::WriteTextFile(*m_sd, kSettingsPath, DumpSettings(settings), &error)) {
        Trace("settings: could not be saved: %s", error.c_str());
    }
}

/* Hash of our payload inside the install record (empty when nothing is recorded). */
std::string App::InstalledPayloadHash() const {
    for (const auto &file : m_state.files) {
        if (file.target.find("subsdk9") != std::string::npos) {
            return file.sha256;
        }
    }
    return std::string();
}

/* Hash of the payload the release manifest would install. */
std::string App::ManifestPayloadHash() const {
    if (m_plan.game == nullptr) {
        return std::string();
    }
    for (const auto &file : m_plan.game->files) {
        if (file.name == "subsdk9") {
            return file.sha256;
        }
    }
    return std::string();
}

void App::RefreshPlan() {
    const manifest::Manifest &active = ActiveManifest();
    if (m_have_manifest || m_have_update_manifest) {
        m_gate = install::Evaluate(&active, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, active.agent);
    } else {
        m_gate = install::Evaluate(nullptr, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, manifest::AgentInfo{});
    }
    VerifyRecordAgainstCard();
}

/* The record is what the installer *did*; the card is what *is*.  `Plan()` only compares the
   record against the manifest, so this is the step that notices someone deleted or renamed the
   game directory while the app was closed -- otherwise the home screen keeps saying "installed"
   (measured: renaming `atmosphere/contents/<title>/exefs` over FTP while the app was closed
   left the old verdict on screen). */
void App::VerifyRecordAgainstCard() {
    m_files_incomplete = false;
    if (!m_have_state || m_plan.action != install::PlanAction::UpToDate) {
        return; /* nothing recorded, or the plan already asks for an install / repair */
    }
    std::string reason;
    if (install::VerifyInstalledFiles(*m_sd, m_state, &reason)) {
        return;
    }
    m_files_incomplete = true;
    Trace("collect: the card does not match the record: %s", reason.c_str());
    /* Same action, honest reason: "repair" is what the button offers, and the home screen gives
       it the wording this case deserves (see HomeKind::Incomplete). */
    m_plan.action = install::PlanAction::Repair;
    m_plan.reason = reason;
}

/* Fold everything the app knows into the single home-screen state. */
void App::UpdateHomeState() {
    HomeInputs in;
    /* Presence is what the content table says, not the version number: a bare 1.0.0 copy is
       installed and carries title version 0, and asking "version != 0" here reported exactly
       that console as "the game was not found". */
    in.game_found = !m_report.build.title_id.empty();
    in.supported = m_gate.status == install::GateStatus::Supported;
    in.last_failed = m_last_failed;
    in.files_incomplete = m_files_incomplete;
    in.repair_needed = m_plan.action == install::PlanAction::Repair;
    /* "Install" with a record present is an update, not a fresh install: the home screen must
       not claim "nothing is installed yet" to someone who has an older agent. */
    in.fresh_install = m_plan.action == install::PlanAction::Install && !m_have_state;
    in.newer_agent = !NewerAgentVersion().empty();
    m_home_kind = Classify(in);
    if (m_trace_frames > 0) {
        Trace("home: kind=%d (found=%d supported=%d failed=%d repair=%d fresh=%d newer=%d)",
              static_cast<int>(m_home_kind), in.game_found ? 1 : 0, in.supported ? 1 : 0,
              in.last_failed ? 1 : 0, in.repair_needed ? 1 : 0, in.fresh_install ? 1 : 0,
              in.newer_agent ? 1 : 0);
    }
}

void App::ToggleLanguage() {
    m_language = m_language == i18n::Language::ZhHans ? i18n::Language::English
                                                     : i18n::Language::ZhHans;
    /* The install engine and the network layer build user-visible messages too, so they read
       the same process-wide language; and the collected report carries sentences built at
       Collect() time (the override advice), so re-collect to rebuild them. */
    i18n::SetLanguage(m_language);
    /* Remember it: the language used to live only in memory, so it was back to Chinese after
       every restart (reported from the console).  A failure here is logged and nothing more --
       the switch itself already took effect. */
    SaveSettings();
    Collect();
}

/* Update check, on the worker thread: `StartUpdateCheck()` only asks for one (the worker does
   the network), and `PollUpdateCheck()` -- called once per frame -- moves a finished result into
   the UI.  The interface therefore stays alive while the request is in flight. */
void App::StartUpdateCheck(bool silent) {
    m_update_silent = silent;
    const std::string start_error = m_update_task.Start(net::kDefaultManifestUrl);
    m_update_running = m_update_task.Running();
    if (!start_error.empty()) {
        /* No worker means no check: report it like any other failure, so the button never looks
           like it simply ignored the press (the raw text lands in the log and the details page). */
        net::UpdateCheckResult failed;
        failed.outcome = net::UpdateOutcome::WorkerFailed;
        failed.detail = start_error;
        ApplyUpdateCheck(std::move(failed));
        return;
    }
    Trace("update check: started (url=%s)", net::kDefaultManifestUrl);
}

void App::PollUpdateCheck() {
    net::UpdateCheckResult result;
    if (m_update_task.Take(&result)) {
        m_update_running = false;
        ApplyUpdateCheck(std::move(result));
        return;
    }
    m_update_running = m_update_task.Running();
}

void App::ApplyUpdateCheck(net::UpdateCheckResult result) {
    m_update_have_result = true;
    m_update_outcome = result.outcome;
    m_update_http_code = result.http_code;
    m_update_detail = result.detail;
    m_update_remote_version.clear();
    m_update_newer_unsupported = false;
    m_have_update_manifest = false;
    m_update_manifest = manifest::Manifest{};
    if (result.outcome == net::UpdateOutcome::Ok) {
        m_update_remote_version = result.agent_version;
        /* What this console has: the record's version, or the version this build carries when
           nothing is installed yet. */
        const std::string installed = m_have_state ? m_state.agent_version : std::string();
        const std::string current =
            !installed.empty() ? installed : (m_have_manifest ? m_manifest.agent.version : installed);
        const auto parsed = manifest::Parse(result.manifest_text, kAppVersion);
        if (parsed.ok) {
            /* The rule lives in net/update_policy.hpp (pure, host-tested): a newer release is
               only adopted when the gate accepts *this* build. */
            const auto decision = net::DecideAdoption(parsed.manifest, m_report.build, current);
            if (decision.adopt) {
                m_update_manifest = parsed.manifest;
                m_have_update_manifest = true;
            }
            m_update_newer_unsupported = decision.unsupported;
        }
    }
    Trace("update check: %s", m_update_detail.c_str());
    RefreshPlan();
    UpdateHomeState();
}

/* The version a press of the primary button would install, when that is newer than what this
   console actually has.  Empty means "nothing newer to offer": the home screen then keeps its
   normal states (fresh install, repair, retry, up to date). */
std::string App::NewerAgentVersion() const {
    const std::string available = ActiveManifest().agent.version;
    const std::string installed = m_have_state ? m_state.agent_version : std::string();
    if (available.empty() || installed.empty()) {
        return {}; /* nothing to compare against: that is not an update story */
    }
    return manifest::CompareVersions(available, installed) > 0 ? available : std::string();
}

const manifest::Manifest &App::ActiveManifest() const {
    if (m_have_update_manifest) {
        return m_update_manifest;
    }
    return m_manifest;
}

/* The check button's second line: idle, in flight, or the outcome -- built here, at render
   time, so it is always in the current language. */
std::string App::UpdateSubtitle() const {
    if (m_update_running) {
        return std::string(Tr(i18n::StringId::SubUpdateChecking));
    }
    if (!m_update_have_result) {
        return std::string(Tr(i18n::StringId::SubCheckUpdate));
    }
    /* The startup check is silent about failure: a machine that is offline when the app opens
       should not greet the player with an error they did not ask for.  The details page and the
       log still carry the reason. */
    if (m_update_silent && m_update_outcome != net::UpdateOutcome::Ok) {
        return std::string(Tr(i18n::StringId::SubCheckUpdate));
    }
    switch (m_update_outcome) {
        case net::UpdateOutcome::Ok:
            /* A newer release that does not cover this build is the one case where the player
               needs to know *why* there is nothing to install: the release host moved on, this
               console's game build did not. */
            if (m_update_newer_unsupported) {
                return i18n::Format(i18n::StringId::SubUpdateNewerUnsupported,
                                    m_update_remote_version.c_str());
            }
            /* With nothing installed there is no "your version" to compare against, so the
               line states what the release host publishes instead of claiming "up to date". */
            if (!m_have_state) {
                return i18n::Format(i18n::StringId::SubUpdatePublished,
                                    m_update_remote_version.c_str());
            }
            return NewerAgentVersion().empty()
                       ? i18n::Format(i18n::StringId::SubUpdateUpToDate,
                                      m_update_remote_version.c_str())
                       : i18n::Format(i18n::StringId::SubUpdateNewer,
                                      m_update_remote_version.c_str());
        case net::UpdateOutcome::HttpStatus:
            return i18n::Format(
                i18n::StringId::SubUpdateFailed,
                i18n::Format(i18n::StringId::UpdateShortHttp, m_update_http_code).c_str());
        case net::UpdateOutcome::NoSignature:
        case net::UpdateOutcome::BadSignature:
        case net::UpdateOutcome::NoTrustAnchor:
            return i18n::Format(i18n::StringId::SubUpdateFailed,
                                Tr(i18n::StringId::UpdateShortSignature));
        case net::UpdateOutcome::InvalidManifest:
            return i18n::Format(i18n::StringId::SubUpdateFailed,
                                Tr(i18n::StringId::UpdateShortManifest));
        case net::UpdateOutcome::WorkerFailed:
            return i18n::Format(i18n::StringId::SubUpdateFailed,
                                Tr(i18n::StringId::UpdateShortInternal));
        case net::UpdateOutcome::NotHttps:
        case net::UpdateOutcome::Network:
            break;
    }
    return i18n::Format(i18n::StringId::SubUpdateFailed,
                        Tr(i18n::StringId::UpdateShortOffline));
}

/* Details page: the raw outcome, so "it failed" always comes with what actually happened. */
std::string App::UpdateDetailText() const {
    if (m_update_running) {
        return std::string(Tr(i18n::StringId::SubUpdateChecking));
    }
    if (!m_update_have_result) {
        return std::string(Tr(i18n::StringId::ValueNotChecked));
    }
    if (m_update_outcome == net::UpdateOutcome::Ok) {
        return m_update_newer_unsupported
                   ? i18n::Format(i18n::StringId::SubUpdateNewerUnsupported,
                                  m_update_remote_version.c_str())
                   : i18n::Format(i18n::StringId::UpdateCheckOk, m_update_remote_version.c_str());
    }
    return i18n::Format(i18n::StringId::UpdateCheckFailed, m_update_detail.c_str());
}

void App::RunInstall() {
    const manifest::Manifest &active = ActiveManifest();
    if ((!m_have_manifest && !m_have_update_manifest) ||
        m_plan.action == install::PlanAction::Blocked || m_plan.game == nullptr) {
        m_result_ok = false;
        m_last_failed = true;
        /* Even when the engine is never reached, the result page has to say whether anything
           was written -- otherwise dry-run would show up as dry_run=no. */
        m_result_files = 0;
        m_result_error = (m_have_manifest || m_have_update_manifest)
                             ? m_plan.reason
                             : Tr(i18n::StringId::ResultNoManifest);
        m_page = Page::Result;
        return;
    }
    /* Payload matches the manifest's origin: the embedded manifest uses the embedded payload, a
       dev manifest uses the payload directory on the SD card, and a verified remote release is
       downloaded from the URLs it names (the engine verifies each file against the manifest
       before anything is written). */
    payload::EmbeddedPayloadSource embedded_source;
    install::SdFolderPayloadSource sd_source(*m_sd, m_payload_dir);
    install::NetworkPayloadSource network_source(active, *m_plan.game);
    install::PayloadSource *source = nullptr;
    if (m_have_update_manifest) {
        source = &network_source;
    } else if (m_manifest_embedded) {
        source = &embedded_source;
    } else {
        source = &sd_source;
    }
    /* Development switch: record every fs call the engine makes, so a failure can be read back
       with the pointer and memory state the kernel saw at that moment. */
    if (DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        install::SetFsTraceSink(m_log);
    }
    /* The install runs on this thread, so the interface cannot animate while it works -- but it
       can (and must) say what it is doing: one frame before the first byte moves, and one after
       every file.  Without this the player stares at the confirmation page for as long as the
       download takes, which reads as "the app hung" (that is the whole subject of
       docs/architecture.md 9.3). */
    BeginProgress(/*uninstall=*/false, static_cast<int>(m_plan.game->files.size()));
    const auto result = install::Install(*m_sd, active, *m_plan.game, *source, /*dry_run=*/false,
                                        [this](const install::Progress &progress) {
                                            UpdateProgress(progress);
                                        });
    install::SetFsTraceSink(nullptr);
    m_result_ok = result.ok;
    m_last_failed = !result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    m_result_uninstall = false;
    /* The reason only used to exist on screen: a failure reported by the user could not be
       looked up in log.txt afterwards, which is exactly what happened while running the
       installer fault-injection tests. */
    Trace("install: ok=%d files=%d%s%s", result.ok ? 1 : 0, result.files_written,
          result.error.empty() ? "" : " error=", result.error.c_str());
    /* Development switch: capture what the fs session can still do, in the state that just
       failed, instead of guessing from a later (healthy) run. */
    if (!result.ok && DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        probe::RunFsSessionProbe(*m_log, *m_sd, "after install failure");
    }
    if (result.ok) {
        /* The record now describes the version we just installed; the remote release is no
           longer "newer than what is installed", so the next Collect() drops it (and with it the
           update button). */
        m_have_update_manifest = false;
        Collect();
    }
    m_page = Page::Result;
}

void App::RunUninstall() {
    if (DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        install::SetFsTraceSink(m_log);
    }
    BeginProgress(/*uninstall=*/true, static_cast<int>(m_state.files.size()));
    const auto result = install::Uninstall(*m_sd, /*dry_run=*/false,
                                           [this](const install::Progress &p) {
                                               UpdateProgress(p);
                                           });
    install::SetFsTraceSink(nullptr);
    /* The result page words itself from these two flags; without this the page claimed
       "install succeeded" right after removing the files. */
    m_result_uninstall = true;
    m_result_ok = result.ok;
    m_last_failed = !result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    Trace("uninstall: ok=%d files=%d%s%s", result.ok ? 1 : 0, result.files_written,
          result.error.empty() ? "" : " error=", result.error.c_str());
    if (!result.ok && DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        probe::RunFsSessionProbe(*m_log, *m_sd, "after uninstall failure");
    }
    if (result.ok) {
        Collect();
    }
    m_page = Page::Result;
}

/* Stick half of the input model: the same rows the d-pad walks, with a repeat so a long list can
   be scrolled without hammering the stick.  Dead zone first (a worn stick must not drift the
   focus), then an immediate first step, then a faster repeat while it stays pushed. */
void App::HandleStick(const PadState &pad) {
    constexpr int kDeadZone = 14000;   /* out of 32767 */
    constexpr u64 kFirstDelayMs = 320; /* before the first repeat */
    constexpr u64 kRepeatMs = 110;
    const HidAnalogStickState stick = padGetStickPos(&pad, 0);
    const bool up = stick.y > kDeadZone;
    const bool down = stick.y < -kDeadZone;
    if (!up && !down) {
        m_stick_next_tick = 0; /* centred: the next push steps immediately */
        return;
    }
    const u64 now = static_cast<u64>(armTicksToNs(armGetSystemTick()) / 1000000ULL);
    if (m_stick_next_tick != 0 && now < m_stick_next_tick) {
        return;
    }
    if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Files) {
        m_file_list.MoveFocus(up ? -1 : +1);
    } else if (m_page == Page::Home) {
        m_focus = MoveFocus(m_actions, m_focus, 0, up ? -1 : +1);
    } else {
        return; /* the page has no vertical focus to move */
    }
    m_stick_next_tick = now + (m_stick_next_tick == 0 ? kFirstDelayMs : kRepeatMs);
}

/* Keyboard half of the input model.  Home has three actions with dedicated keys (A for the
   primary one, X and Y for the two secondary ones) plus focus movement for the d-pad, the
   details page uses focus and A, and B always means "back" (or exit on Home). */
void App::HandleKeys(u32 down) {
    /* While the engine is working, the progress page owns the screen and the thread: no key can
       leave it (the install is synchronous, so in practice nothing arrives here at all -- this
       guard is what keeps that true if the install ever moves to a worker). */
    if (m_page == Page::Progress) {
        return;
    }
    /* The tabs carry their own key, so L / R go through the same action as a tap on them. */
    if ((down & HidNpadButton_L) != 0) {
        ActivateAction(kActionTabStatus);
        return;
    }
    if ((down & HidNpadButton_R) != 0) {
        ActivateAction(kActionTabDetails);
        return;
    }
    if ((down & HidNpadButton_B) != 0) {
        if (m_page == Page::Home) {
            m_should_exit = true; /* B on the home screen leaves the app, as before */
            return;
        }
        /* Leaving the guide or the installer goes back to whatever it was opened from;
           everywhere else B is "back to the status page". */
        if (m_page == Page::Guide) {
            m_page = m_guide_return;
        } else if (m_page == Page::GameInstall) {
            m_page = m_game_install_return;
        } else {
            m_page = Page::Home;
        }
        m_focus = FirstEnabled(m_actions);
        return;
    }
    if ((down & HidNpadButton_Plus) != 0) {
        /* + is the game installer, from any page: it is modal, so B is the only way back and the
           page hides the header entries while it is up. */
        OpenGameInstall();
        return;
    }
    if ((down & HidNpadButton_Minus) != 0) {
        /* - is the chat-code explainer, from any page: the first press opens it, the next one
           (or B) goes back.  Exiting stays on B, which is where the footer points. */
        ToggleGuide();
        return;
    }
    if (m_page == Page::Home) {
        const int step = 1;
        if ((down & HidNpadButton_Left) != 0) {
            m_focus = MoveFocus(m_actions, m_focus, -step, 0);
        } else if ((down & HidNpadButton_Right) != 0) {
            m_focus = MoveFocus(m_actions, m_focus, +step, 0);
        } else if ((down & HidNpadButton_Up) != 0) {
            m_focus = MoveFocus(m_actions, m_focus, 0, -step);
        } else if ((down & HidNpadButton_Down) != 0) {
            m_focus = MoveFocus(m_actions, m_focus, 0, +step);
        } else if ((down & HidNpadButton_A) != 0) {
            /* A has a fixed meaning on this page (the primary action).  It must not go through
               the focus: key-bound controls are no longer focus targets, so the focus is -1
               here and "A stopped working" would come straight back. */
            ActivateAction(kActionPrimary);
            return;
        } else if ((down & HidNpadButton_X) != 0) {
            ActivateAction(kActionCheckUpdate);
            return;
        } else if ((down & HidNpadButton_Y) != 0) {
            ActivateAction(kActionUninstall);
            return;
        }
        /* Focus may have landed on a disabled action (e.g. "already up to date"); A then does
           nothing, which is what a greyed-out button should do. */
        return;
    }
    switch (m_page) {
        case Page::Details:
            if ((down & HidNpadButton_A) != 0) {
                /* The only switchable row today is the language (the same action a tap on the
                   row uses); the dev manifest toggle is a development aid and stays on Y. */
                ActivateAction(kActionLanguage);
            } else if ((down & HidNpadButton_Y) != 0) {
                m_allow_dev_manifest = !m_allow_dev_manifest;
                Collect();
            }
            break;
        case Page::GameInstall:
            /* Two sub-screens, two key maps: the picker moves with up/down and picks with A, the
               source screen uses Y/X/A for the three sources.  B is "one level back" in both, and
               leaves the modal page from the source screen. */
            if (m_install_screen == InstallScreen::Progress) {
                break; /* the engine owns the thread: no key does anything here */
            }
            if (m_install_screen == InstallScreen::Confirm) {
                if ((down & HidNpadButton_A) != 0) {
                    RunGameInstall();
                } else if ((down & HidNpadButton_B) != 0) {
                    m_install_screen = InstallScreen::Choose;
                    m_focus = FirstEnabled(m_actions);
                }
                break;
            }
            if (m_install_screen == InstallScreen::Files) {
                if ((down & HidNpadButton_B) != 0) {
                    m_install_screen = InstallScreen::Choose;
                    m_focus = FirstEnabled(m_actions);
                } else if ((down & HidNpadButton_Y) != 0) {
                    LeaveDirectory();
                } else if ((down & HidNpadButton_A) != 0) {
                    PickFocusedFile();
                } else if ((down & HidNpadButton_Up) != 0) {
                    m_file_list.MoveFocus(-1);
                } else if ((down & HidNpadButton_Down) != 0) {
                    m_file_list.MoveFocus(+1);
                }
                break;
            }
            if ((down & HidNpadButton_Y) != 0) {
                ActivateAction(kActionInstallLocal);
            } else if ((down & HidNpadButton_X) != 0) {
                ActivateAction(kActionInstallDownload);
            } else if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionInstallUpload);
            } else if ((down & HidNpadButton_B) != 0) {
                m_page = m_game_install_return;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        case Page::Install:
            if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionPrimary);
            } else if ((down & HidNpadButton_B) != 0) {
                ActivateAction(kActionBack);
            }
            break;
        case Page::Uninstall:
            if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionPrimary);
            } else if ((down & HidNpadButton_B) != 0) {
                ActivateAction(kActionBack);
            }
            break;
        case Page::Result:
            if ((down & HidNpadButton_A) != 0) {
                ActivateAction(kActionPrimary);
            }
            break;
        default:
            break;
    }
}

/* Touch half: press remembers the action under the finger, release inside the same action
   fires it (drag cancels).  TapTracker owns the rule so the host tests can pin it. */
void App::HandleTouch() {
    int x = 0;
    int y = 0;
    int count = 0;
    const bool down = m_touch.Poll(&x, &y, &count);
    const TapResult tap = m_tap.Update(m_actions, down, x, y);
    if (tap.press_edge && tap.pressed_action >= 0) {
        m_focus = tap.pressed_action;
    }
    if (tap.tapped_action >= 0) {
        ActivateAction(tap.tapped_action);
    }
}

/* One place that turns an action id into behaviour, so keys and touch cannot drift apart.
   Several ids are shared by the pages (the primary button, "back"), so the page decides what
   they mean.  Tapping "install now" on the confirmation page used to re-run the *home* page's
   meaning of the primary action, which silently did nothing. */
void App::ActivateAction(int id) {
    switch (m_page) {
        case Page::Install:
            if (id == kActionPrimary) {
                RunInstall();
                return;
            }
            if (id == kActionBack) {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
                return;
            }
            break;
        /* The progress page takes no input at all (the engine owns the thread) and the guide
           page is read-only: both fall through to the shared handling below.  The installer's own
           actions (its cards and its list rows) are page-specific and handled there too. */
        case Page::Progress:
        case Page::Guide:
        case Page::GameInstall:
            break;
        case Page::Uninstall:
            if (id == kActionPrimary) {
                RunUninstall();
                return;
            }
            if (id == kActionBack) {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
                return;
            }
            break;
        case Page::Result:
            /* "Done" is the only control here: refresh the state and go back to the status
               page (the key path did this; the touch path forgot to). */
            if (id == kActionPrimary) {
                Collect();
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
                return;
            }
            break;
        case Page::Details:
            if (id == kActionLanguage) {
                ToggleLanguage();
                return;
            }
            break;
        case Page::Home:
            break;
    }
    switch (id) {
        case kActionPrimary:
            switch (m_home_kind) {
                case HomeKind::GameMissing:
                case HomeKind::UpToDate:
                    Collect(); /* recheck / refresh */
                    break;
                case HomeKind::Unsupported:
                    /* The one useful thing to do here: offer the installer, because a package the
                       player brings is how this state stops being true. */
                    OpenGameInstall();
                    break;
                case HomeKind::NeedsInstall:
                case HomeKind::UpdateAvailable:
                case HomeKind::Repair:
                case HomeKind::Incomplete:
                case HomeKind::Failed:
                    m_page = Page::Install;
                    break;
            }
            break;
        case kActionCheckUpdate: StartUpdateCheck(); break;
        case kActionGuide: ToggleGuide(); break;
        case kActionGameInstall: OpenGameInstall(); break;
        /* The three source screens land here next milestone; the entries are drawn and pressable
           already so the page's layout and key map can be tested on hardware. */
        case kActionInstallLocal:
            m_install_screen = InstallScreen::Files;
            m_file_dir = "/";
            RefreshFileList();
            break;
        case kActionInstallPickRow: PickFocusedFile(); break;
        case kActionInstallStart: RunGameInstall(); break;
        case kActionInstallDownload: Trace("install: source = download"); break;
        case kActionInstallUpload: Trace("install: source = upload"); break;
        case kActionUninstall:
            if (m_page == Page::Home && m_have_state) {
                m_page = Page::Uninstall;
            }
            break;
        case kActionBack:
            m_page = Page::Home;
            break;
        /* Header tabs: L shows the status page, R the details page.  Switching pages rebuilds
           the action list, so the focus is re-seated on whatever the new page offers. */
        case kActionTabStatus:
            if (m_page != Page::Home) {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        case kActionTabDetails:
            if (m_page != Page::Details) {
                m_page = Page::Details;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        /* The footer's Ⓑ hint does exactly what the B key does. */
        case kActionFooterBack:
            if (m_page == Page::Home) {
                m_should_exit = true;
            } else {
                m_page = Page::Home;
                m_focus = FirstEnabled(m_actions);
            }
            break;
        default:
            break;
    }
}

void App::Run() {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    m_touch.Init(m_log);
    /* The game installer writes its diagnostics through this sink (one line each); it has nothing
       to do with the agent install path, so it is set here, once, rather than in RunInstall. */
    install::SetGameInstallLog(m_log);
    m_focus = FirstEnabled(m_actions);
    while (appletMainLoop() && !m_should_exit) {
        PollUpdateCheck();
        padUpdate(&pad);
        HandleKeys(padGetButtonsDown(&pad));
        HandleStick(pad);
        HandleTouch();
        Render();
    }
}

/* First-frame stage pauses: wait for + before continuing (used to localize a render crash
   to one stage). */
void App::WaitForPlus(const char *stage) {
    Trace("frame: paused after %s (press + to continue)", stage);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
            break;
        }
    }
    Trace("frame: resuming after %s", stage);
}

void App::Render() {
    if (!m_fb_ready) {
        return;
    }
    u32 stride = 0;
    /* After framebufferMakeLinear you must draw on the shadow buffer returned by
       framebufferBegin; framebufferEnd copies it into the real framebuffer. */
    void *framebuffer = framebufferBegin(&m_fb, &stride);
    const bool trace = m_trace_frames > 0;
    if (trace) {
        Trace("frame: begin (buf=%p stride=%u)", framebuffer, stride);
    }
    Surface surface;
    surface.pixels = static_cast<u32 *>(framebuffer != nullptr ? framebuffer : m_fb.buf);
    surface.width = static_cast<int>(m_fb.width_aligned);
    /* Important: framebufferMakeLinear sizes the linear shadow buffer by the **window
       height** (stride * ((win->height+7)&~7)), while width/height_aligned round up to the
       GOB boundary (720 -> 768 rows).  Drawing at height_aligned writes 48 rows past the
       buffer (about 245 KB per frame) into the process heap shared with the hbl loader,
       which measurably crashed the loader in its stdio buffer path.  So the drawing height
       is the smaller of the window height and height_aligned. */
    surface.height = std::min(static_cast<int>(m_fb.height_aligned),
                              static_cast<int>(m_fb.win->height));
    surface.stride = static_cast<int>(stride / 4);

    Fill(surface, kBackground);
    if (trace) {
        Trace("frame: fill done");
        if (m_stage_pause) {
            WaitForPlus("fill");
        }
    }
    RenderHeader(surface);
    if (trace) {
        Trace("frame: header done");
        if (m_stage_pause) {
            WaitForPlus("header");
        }
    }
    switch (m_page) {
        case Page::Home: RenderHome(surface); break;
        case Page::GameInstall: RenderGameInstall(surface); break;
        case Page::Details: RenderDetails(surface); break;
        case Page::Install: RenderInstall(surface); break;
        case Page::Uninstall: RenderUninstall(surface); break;
        case Page::Progress: RenderProgress(surface); break;
        case Page::Guide: RenderGuide(surface); break;
        case Page::Result: RenderResult(surface); break;
    }
    /* Every page shows the same chrome (header tabs, footer hint), added after the page's own
       controls so the entry focus stays on the page's primary action. */
    AddChromeActions(surface.width, surface.height);
    /* The list is rebuilt every frame, so the focus can end up pointing at an action this page
       does not have: switching pages re-seats it from the *old* list, and the details page's
       first action is its language row.  A focus id that is not in the list makes A do nothing
       -- the "A stopped working after pressing R then L" bug.  A disabled control is left alone
       on purpose: the ring stays on it and A does nothing, which is what a greyed-out button
       should do. */
    {
        bool focus_known = false;
        for (const Action &action : m_actions) {
            if (action.id == m_focus) {
                focus_known = true;
                break;
            }
        }
        if (!focus_known) {
            m_focus = FirstEnabled(m_actions);
        }
    }
    if (trace) {
        Trace("frame: body done");
        if (m_stage_pause) {
            WaitForPlus("body");
        }
    }
    RenderFooter(surface);
    if (trace) {
        Trace("frame: footer done");
        if (m_stage_pause) {
            WaitForPlus("footer");
        }
    }
    framebufferEnd(&m_fb);
    if (trace) {
        Trace("frame: end done");
        --m_trace_frames;
    }
}

void App::RenderHeader(Surface surface) {
    FillRect(surface, 0, 0, surface.width, kHeaderHeight, kHeader);
    /* The title's descenders (the 'g' in Manager) reach about 9 px below the baseline, so the
       subtitle starts below them instead of touching them. */
    m_font.Draw(surface, kMargin, 10, kFontTitle, kOnHeader, Tr(i18n::StringId::AppTitle));
    /* Subtitle: what this program is, for the people who just installed it. */
    m_font.Draw(surface, kMargin + 2, 66, kFontSmall, kHeaderSubtle,
                i18n::Format(i18n::StringId::HeadSubtitle, "3.0.3"));
    /* Tabs carry their own key, so "one button, one action" holds here too. */
    /* No tabs while the engine works: the progress page ignores input (see HandleKeys), and a
       drawn control that cannot be pressed is exactly what this project keeps fixing. */
    if (m_page == Page::Progress) {
        return;
    }
    /* The installer page is the app's one modal page: the four entries are hidden (not greyed
       out) and the room they took shows the two storage bars, so the player sees why the page
       cannot be left with L/R and how much room the install has. */
    if (m_page == Page::GameInstall) {
        RenderStorageBars(surface);
        return;
    }
    struct Tab {
        const char *key;
        i18n::StringId label;
        int action;
    };
    /* Order matters twice: it is what the player reads left to right, and the device tests
       identify the current page by which entry carries the underline. */
    const Tab tabs[kHeaderTabCount] = {{"L", i18n::StringId::TabStatus, kActionTabStatus},
                                       {"R", i18n::StringId::TabDetails, kActionTabDetails},
                                       {"+", i18n::StringId::TabInstall, kActionGameInstall},
                                       {"-", i18n::StringId::TabGuide, kActionGuide}};
    int label_width[kHeaderTabCount] = {};
    for (int i = 0; i < kHeaderTabCount; ++i) {
        label_width[i] = m_font.Measure(Tr(tabs[i].label), kFontHeading);
    }
    const HeaderTabLayout layout = LayoutHeaderTabs(surface.width, label_width);
    for (int i = 0; i < kHeaderTabCount; ++i) {
        /* Which entry reads as "where I am": the guide while it is open, otherwise status
           everywhere except the details page. */
        bool active = false;
        switch (tabs[i].action) {
            case kActionGuide: active = m_page == Page::Guide; break;
            case kActionGameInstall: active = m_page == Page::GameInstall; break;
            case kActionTabStatus:
                active = m_page != Page::Details && m_page != Page::Guide;
                break;
            case kActionTabDetails: active = m_page == Page::Details; break;
            default: break;
        }
        const char *label = Tr(tabs[i].label);
        m_font.Draw(surface, layout.label_x[i], 36, kFontHeading,
                    active ? kOnHeader : kHeaderSubtle, label);
        FillRoundedRect(surface, layout.badge_x[i], kHeaderTabTop, kHeaderTabBadge,
                        kHeaderTabBadge, kHeaderTabBadge / 2,
                        active ? kOnHeader : kHeaderBadge);
        /* Centre the letter on both axes: our Draw() treats y as the top of the line box, so
           bias it by half the box to land the glyph in the middle of the circle. */
        const int key_width = m_font.Measure(tabs[i].key, kFontSmall);
        m_font.DrawBold(surface, layout.badge_x[i] + (kHeaderTabBadge - key_width) / 2,
                        kHeaderTabTop + 10, kFontSmall, active ? kHeader : kOnHeader,
                        tabs[i].key);
        if (active) {
            FillRect(surface, layout.badge_x[i] + kHeaderTabBadge + kHeaderTabBadgeGap,
                     kHeaderHeight - 14, label_width[i], 4, kOnHeader);
        }
    }
}

/* Everything the chrome draws as a button is one: the header tabs and the footer's Ⓑ hint go
   through the same actions as the L / R / B keys (see HandleKeys -> ActivateAction), and the
   drawn pill and the touch target come from one layout function each.  They are appended
   after the page's own controls, which keeps the entry focus on the page's primary action. */
void App::AddChromeActions(int surface_width, int surface_height) {
    /* The progress page has no chrome on purpose: its keys are dead while the engine works, and
       drawing tabs or a "B back" hint would promise controls that do nothing. */
    if (m_page == Page::Progress || m_page == Page::GameInstall) {
        return;
    }
    const i18n::StringId header_labels[kHeaderTabCount] = {
        i18n::StringId::TabStatus, i18n::StringId::TabDetails, i18n::StringId::TabInstall,
        i18n::StringId::TabGuide};
    const int header_actions[kHeaderTabCount] = {kActionTabStatus, kActionTabDetails,
                                                 kActionGameInstall, kActionGuide};
    int label_width[kHeaderTabCount] = {};
    for (int i = 0; i < kHeaderTabCount; ++i) {
        label_width[i] = m_font.Measure(Tr(header_labels[i]), kFontHeading);
    }
    const HeaderTabLayout layout = LayoutHeaderTabs(surface_width, label_width);
    for (int i = 0; i < kHeaderTabCount; ++i) {
        m_actions.push_back({header_actions[i], layout.hit[i], true});
    }

    if (!HasOwnBackButton()) {
        const i18n::StringId label_id =
            m_page == Page::Home ? i18n::StringId::LabelExit : i18n::StringId::LabelBack;
        const int footer_label = m_font.Measure(Tr(label_id), kFontSmall);
        const FooterHintLayout footer = LayoutFooterHint(
            surface_height, m_font.LineHeight(kFontSmall), footer_label);
        m_actions.push_back({kActionFooterBack, footer.hit, true});
    }
}

void App::RenderFooter(Surface surface) {
    const int y = surface.height - kFooterHeight;
    FillRect(surface, 0, y, surface.width, kFooterHeight, kCard);
    StrokeRect(surface, 0, y, surface.width, 2, 2, kBorder);
    /* The footer keeps one thing the buttons cannot show: how to leave.  B gets the same kind
       of badge the buttons use, so the "one button, one action" rule still holds. */
    /* Two screens of the install flow say something extra here, because the player has to know it
       before pressing anything:
         * the transfer runs on this thread and takes no keys at all, so promising a "back" would
           be a lie -- the footer asks to stay instead (`Y`/`B` cancel on a worker thread, which is
           a later piece of work; see the design doc);
         * the confirm screen can be started with `Ⓐ`, and that hint belongs next to "leave". */
    if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Progress) {
        const char *hint = Tr(i18n::StringId::ProgressDoNotInterrupt);
        const int hint_width = m_font.Measure(hint, kFontSmall);
        m_font.Draw(surface, (surface.width - hint_width) / 2, y + 12, kFontSmall, kWarn, hint);
        return;
    }
    if (m_page == Page::GameInstall && m_install_screen == InstallScreen::Confirm) {
        const char *start = Tr(i18n::StringId::ConfirmStart);
        FillRoundedRect(surface, kMargin, y + 7, 30, 30, 15, kHeader);
        const int key_width = m_font.Measure("A", kFontSmall);
        m_font.DrawBold(surface, kMargin + (30 - key_width) / 2, y + 13, kFontSmall, kOnHeader,
                        "A");
        m_font.Draw(surface, kMargin + 42, y + 12, kFontSmall, kText, start);
    }
    /* The confirmation and result pages carry their own Ⓑ button, so the footer stays quiet
       there: one key, one place. */
    const bool has_back_button = HasOwnBackButton();
    const i18n::StringId label_id =
        m_page == Page::Home ? i18n::StringId::LabelExit : i18n::StringId::LabelBack;
    const char *label = has_back_button ? "" : Tr(label_id);
    const FooterHintLayout hint = LayoutFooterHint(
        surface.height, m_font.LineHeight(kFontSmall),
        has_back_button ? 0 : m_font.Measure(label, kFontSmall));
    if (!has_back_button) {
        FillRoundedRect(surface, hint.badge_x, hint.badge_y, hint.badge, hint.badge,
                        hint.badge / 2, kBorder);
        const int key_width = m_font.Measure("B", kFontSmall);
        m_font.DrawBold(surface, hint.badge_x + (hint.badge - key_width) / 2, hint.badge_y + 6,
                        kFontSmall, kText, "B");
        /* Measured on the console: the layout's offset puts the label's optical centre on the
           badge's (the glyph box is not the visual box). */
        m_font.Draw(surface, hint.label_x, hint.text_y, kFontSmall, kSubtle, label);
    }
    const int stamp_width = m_font.Measure(kBuildStamp, kFontSmall);
    m_font.Draw(surface, surface.width - kMargin - stamp_width, hint.text_y, kFontSmall, kSubtle,
                kBuildStamp);
}

bool App::HasOwnBackButton() const {
    /* The confirmation and result pages draw their own Ⓑ button, so the footer hint is omitted
       there: one key, one place.  The progress page has no back affordance at all -- B cannot
       leave it while the engine works -- so it stays quiet too. */
    return m_page == Page::Install || m_page == Page::Uninstall || m_page == Page::Result ||
           m_page == Page::Progress;
}

void App::Card(Surface surface, int x, int y, int w, int h, const char *title) {
    FillRect(surface, x, y, w, h, kCard);
    StrokeRect(surface, x, y, w, h, 2, kBorder);
    FillRect(surface, x, y, w, 4, kHeader);
    if (title != nullptr) {
        /* Same left inset as the rows underneath, so the card's title and its content share
           one edge (the 6 px difference was visible on hardware). */
        m_font.Draw(surface, x + kCardPadX, y + 14, kFontHeading, kText, title);
        /* (title sits at y+14; the band is 48 so the first row keeps its clearance) */
    }
}

/* The height one row really occupies: the value wraps at value_width and Fit() truncates
   anything past max_lines, so the measured line count and the painted line count always
   agree. */
int App::RowsHeight(const std::vector<Row> &rows, int value_width, int row_gap) {
    int total = 0;
    bool first = true;
    for (const Row &row : rows) {
        /* A null label means the row is all value and spans the whole card body width. */
        const int width = row.label == nullptr ? value_width + kLabelColumn : value_width;
        const std::string text = m_font.Fit(row.value, row.size, width, row.max_lines);
        total += std::max(1, m_font.LineCount(text, row.size, width)) * m_font.LineHeight(row.size);
        if (!first) {
            total += row_gap;
        }
        first = false;
    }
    return total;
}

/* surface is already the card body's clip region (origin at the card's top-left), so this
   only paints the rows themselves. */
void App::DrawRows(Surface surface, const std::vector<Row> &rows, int value_width, int row_gap) {
    int pen_y = 0;
    bool first = true;
    for (const Row &row : rows) {
        if (!first) {
            pen_y += row_gap;
        }
        first = false;
        const bool full_width = row.label == nullptr;
        const int width = full_width ? value_width + kLabelColumn : value_width;
        const std::string text = m_font.Fit(row.value, row.size, width, row.max_lines);
        if (m_trace_frames > 0) {
            Trace("row: label=\"%s\" value=\"%.24s\"", row.label != nullptr ? row.label : "",
                  text.c_str());
        }
        if (full_width) {
            m_font.Draw(surface, kCardPadX, pen_y, row.size, row.color, text, width);
        } else {
            m_font.Draw(surface, kCardPadX, pen_y, row.size, kSubtle, row.label);
            m_font.Draw(surface, kCardPadX + kLabelColumn, pen_y, row.size, row.color, text, width);
        }
        pen_y +=
            std::max(1, m_font.LineCount(text, row.size, width)) * m_font.LineHeight(row.size);
    }
}

int App::DrawCard(Surface page, int y, int bottom, int width, int value_width, const char *title,
                  const std::vector<Row> &rows, int row_gap, bool fill_rest, int bottom_pad) {
    /* Without a title the card does not need the title band: that empty strip used to sit
       above the rows while the file list below was squeezed. */
    const int top_inset = title != nullptr ? kCardHeader : kCardPadTop;
    int height = top_inset + RowsHeight(rows, value_width, row_gap) + bottom_pad;
    if (fill_rest) {
        height = std::max(height, bottom - y);
    }
    height = std::min(height, bottom - y);
    if (height <= 0) {
        return bottom;
    }
    Card(page, kMargin, y, width, height, title);
    /* The body goes into a subview inside the card: coordinates start at 0 and no row can
       cross the border. */
    const Surface body = page.Subview(kMargin, y + top_inset, width, height - top_inset);
    /* Rows but an empty view means the coordinates or the clip are wrong.  That mistake only
       looks like "an empty card", which is hard to tell from "there really is no data", so
       it gets an explicit log line. */
    if (body.pixels == nullptr && !rows.empty()) {
        Trace("card: body is empty but has %zu rows (title=%s y=%d h=%d)", rows.size(), title, y,
              height);
    }
    DrawRows(body, rows, value_width, row_gap);
    return y + height + kCardGap;
}

/* ---------------- home screen: one screen, one action ---------------- */

namespace {

/* Layout of the home screen (see the design sketch: status line, primary button, two
   half-width blocks).  The content margin is the page margin the header, footer and every
   other page use, so the left/right edges line up across the whole app. */
constexpr int kHomeMargin = kMargin;
constexpr int kHomeStatusY = 136;
constexpr int kHomePrimaryY = 270;
constexpr int kHomeButtonHeight = 144;
constexpr int kHomeSecondaryY = 450;
constexpr int kHomeSecondaryHeight = 110;
constexpr int kHomeGap = 24;

/* The legacy-cheat strip.  Home carries one message that is not a state: a cheat file left in
   the game directory drives the same in-game input path as the agent, so missing that message
   changes what happens in the game.  It therefore sits above the buttons rather than being
   folded into the state line (a healthy install and a leftover cheat file are both true at
   once), and the buttons step down for it instead of the strip being squeezed into the gap.
   The geometry is derived from the status block so a font or copy change cannot make them
   overlap: the status block ends at `status_y + title line + 6 + body line`. */
constexpr int kHomeStatusBlockBottom =
    kHomeStatusY + (kFontTitle + kFontTitle / 3) + 6 + (kFontBody + kFontBody / 3);
/* The line is the path, so it is long: one size below the body text keeps the whole strip on one
   line in both languages (the English label is the longer of the two). */
constexpr int kHomeWarnFont = kFontSmall;
constexpr int kHomeWarnPad = 18; /* above and below the line, inside the border */
constexpr int kHomeWarnHeight =
    (kHomeWarnFont + kHomeWarnFont / 3) + kHomeWarnPad * 2;
constexpr int kHomeWarnY = kHomeStatusBlockBottom + 12;
/* What the two button rows move down while the strip is shown: exactly the room it takes plus the
   gap that keeps it off the primary button. */
constexpr int kHomeWarnShift = (kHomeWarnY + kHomeWarnHeight + 20) - kHomePrimaryY;
static_assert(kHomeWarnShift > 0, "the warning strip has to push the buttons down");

/* Key badge: the physical button that also triggers this control, drawn inside it. */
void DrawKeyBadge(Surface surface, Font &font, int cx, int cy, int radius, const char *key,
                  Color face, Color text) {
    FillRoundedRect(surface, cx - radius, cy - radius, radius * 2, radius * 2, radius, face);
    const int width = font.Measure(key, kFontBody);
    font.DrawBold(surface, cx - width / 2, cy - kFontBody / 2 - 1, kFontBody, text, key);
}

/* Busy indicator: eight dots in a ring, the brightest one walking around, drawn where the key
   badge would be so the button's layout does not move while a check is in flight.  Alpha is
   blended by the drawing layer, so it reads as a spinner on any button face colour. */
void DrawSpinner(Surface surface, int cx, int cy, int radius, Color color) {
    constexpr int kDots = 8;
    constexpr double kTwoPi = 6.283185307179586;
    /* One dot step every 1/8 s, so a revolution takes about a second; the phase comes from the
       clock rather than the frame count, so the speed does not depend on the frame rate. */
    const u64 ticks = armGetSystemTick();
    const u64 phase = (ticks / (armGetSystemTickFreq() / 8)) % kDots;
    for (int i = 0; i < kDots; ++i) {
        const int step = (static_cast<int>(phase) + i) % kDots;
        const double angle = (kTwoPi * step) / kDots - kTwoPi / 4;
        const int x = cx + static_cast<int>(radius * std::cos(angle));
        const int y = cy + static_cast<int>(radius * std::sin(angle));
        const u8 alpha = static_cast<u8>(40 + (215 * (kDots - i)) / kDots);
        FillRoundedRect(surface, x - 3, y - 3, 7, 7, 3, Color{color.r, color.g, color.b, alpha});
    }
}

/* The attention mark of the home warning strip: a filled triangle with an exclamation mark
   knocked out of it.  The drawing layer has no polygon primitive, so the triangle is painted one
   scanline at a time.  `size` is its height and the base is a fifth wider -- an equilateral
   warning triangle is wider than tall, and the first version of this mark (base twice the height)
   read as a stretched blob on the console.  `cut` is the strip's own fill colour, so the mark
   works on any background the strip is given. */
void DrawWarnMark(Surface surface, int cx, int cy, int size, Color color, Color cut) {
    const int top = cy - size / 2;
    const int base = (size * 6) / 5;
    for (int row = 0; row < size; ++row) {
        const int width = ((row + 1) * base) / size;
        FillRect(surface, cx - width / 2, top + row, width, 1, color);
    }
    /* The glyph is centred in the triangle's interior: a stem, a gap, and a dot.  Its rows are
       measured down from the apex so a change of `size` keeps the proportions. */
    constexpr int kStroke = 3;
    constexpr int kDot = 3;
    constexpr int kGap = 3;
    const int stem_height = size / 3;
    const int glyph_height = stem_height + kGap + kDot;
    const int stem_top = top + (size - glyph_height) / 2;
    FillRect(surface, cx - kStroke / 2, stem_top, kStroke, stem_height, cut);
    FillRect(surface, cx - kStroke / 2, stem_top + stem_height + kGap, kStroke, kDot, cut);
}

/* Centre a single line in a rectangle.  The line box is what gets centred, so the ink lands in
   the optical middle -- the old fixed "-26 px" offset left every button label about 10 px high
   (measured on the confirmation page). */
/* Measured on the console: the glyph box carries descender space, so centring the line box
   still leaves the ink about 6 px high inside a 110 px button. */
constexpr int kLabelOpticalBias = 6;

void DrawCenteredLabel(Surface surface, Font &font, const Rect &rect, int size, Color color,
                       const std::string &text) {
    const int width = font.Measure(text.c_str(), size);
    const int line = font.LineHeight(size);
    font.Draw(surface, rect.x + (rect.w - width) / 2,
              rect.y + (rect.h - line) / 2 + kLabelOpticalBias, size, color, text);
}

}  // namespace

void App::BuildHomeActions(int width, int height) {
    (void)height;
    const int inner = width - kHomeMargin * 2;
    const int half = (inner - kHomeGap) / 2;
    const bool installed = m_have_state && m_home_kind != HomeKind::NeedsInstall &&
                           m_home_kind != HomeKind::GameMissing;
    /* A greyed-out primary button means "nothing to do": already up to date, or this build
       cannot be handled at all. */
    /* Unsupported keeps an enabled primary button: it opens the game installer instead of doing
       nothing, which is the whole point of 1.1.0 for a console stuck on 1.0.0. */
    const bool primary_enabled = m_home_kind != HomeKind::UpToDate;
    /* One shift for both rows, and it lives here because the touch targets are built from the
       same rectangles: moving the buttons in the renderer alone would leave the finger hitting
       the old place. */
    const int shift = m_report.legacy_cheat_present ? kHomeWarnShift : 0;
    m_actions.clear();
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, kHomePrimaryY + shift, inner, kHomeButtonHeight},
         primary_enabled});
    /* Without the uninstall button there is nothing to pair with, and a half-width button next
       to empty space reads as a missing control, so the check button takes the full row. */
    const int check_width = installed ? half : inner;
    m_actions.push_back({kActionCheckUpdate,
                         Rect{kHomeMargin, kHomeSecondaryY + shift, check_width,
                              kHomeSecondaryHeight},
                         true});
    if (installed) {
        m_actions.push_back({kActionUninstall,
                             Rect{kHomeMargin + half + kHomeGap, kHomeSecondaryY + shift, half,
                                  kHomeSecondaryHeight},
                             true});
    }
}

void App::RenderHome(Surface surface) {
    BuildHomeActions(surface.width, surface.height);
    /* Colour carries the state: teal = ready to act, green = fine, amber = needs attention,
       red = something failed. */
    Color dot = kHeader;
    switch (m_home_kind) {
        case HomeKind::GameMissing:
        case HomeKind::Unsupported:
        case HomeKind::Incomplete:
        case HomeKind::Repair: dot = kWarn; break;
        case HomeKind::Failed: dot = kBad; break;
        case HomeKind::UpdateAvailable:
        case HomeKind::UpToDate: dot = kGood; break;
        case HomeKind::NeedsInstall: dot = kUninstalled; break;
    }

    /* Headline + the one line of context a player cares about (the two versions). */
    i18n::StringId headline = i18n::StringId::StateNotInstalled;
    switch (m_home_kind) {
        case HomeKind::GameMissing: headline = i18n::StringId::StateGameMissing; break;
        case HomeKind::Unsupported: headline = i18n::StringId::StateUnsupported; break;
        case HomeKind::Failed: headline = i18n::StringId::StateFailed; break;
        case HomeKind::Incomplete: headline = i18n::StringId::StateIncomplete; break;
        case HomeKind::Repair: headline = i18n::StringId::StateRepair; break;
        case HomeKind::NeedsInstall: headline = i18n::StringId::StateNotInstalled; break;
        case HomeKind::UpdateAvailable: headline = i18n::StringId::StateUpdateAvailable; break;
        case HomeKind::UpToDate: headline = i18n::StringId::StateInstalled; break;
    }
    const std::string game_version = InstalledGameVersion();
    /* Two different questions, two different answers: "what is on the card" (the install
       record) and "what a tap would install" (the manifest in use).  They only diverge after a
       network update, and calling the version this build carries "installed" would be wrong. */
    const std::string installed_agent =
        m_have_state ? m_state.agent_version : std::string(Tr(i18n::StringId::ValueNotInstalled));
    const std::string offer_agent =
        (m_have_manifest || m_have_update_manifest)
            ? ActiveManifest().agent.version
            : std::string(Tr(i18n::StringId::ValueNotInstalled));
    std::string second_line;
    switch (m_home_kind) {
        case HomeKind::Repair:
            /* State the facts only, and make the two sides distinguishable: the version alone
               can repeat when only the build changed, so each side carries the first bytes of
               its payload hash -- the same value the installer verifies against. */
            /* Format() is variadic: %s needs a const char*, and the temporary strings live
               until the end of this full expression. */
            second_line = i18n::Format(i18n::StringId::SubRepair,
                                       BuildLabel(m_state.agent_version, InstalledPayloadHash()).c_str(),
                                       BuildLabel(m_manifest.agent.version, ManifestPayloadHash()).c_str());
            break;
        case HomeKind::GameMissing:
            second_line = Tr(i18n::StringId::StateGameMissingSub);
            break;
        case HomeKind::Unsupported: {
            /* "Not supported" on its own leaves the player comparing nothing: name the version
               that is installed and the one this build accepts.  When there is no *different*
               version to name (no manifest, or a refusal that is not about the version), the line
               falls back to pointing at the details page. */
            const std::string supported = SupportedGameVersionNote();
            second_line = supported.empty()
                              ? std::string(Tr(i18n::StringId::StateUnsupportedSub))
                              : i18n::Format(i18n::StringId::HomeVersionsUnsupported,
                                             game_version.c_str(), supported.c_str());
            break;
        }
        case HomeKind::Failed:
            second_line = Tr(i18n::StringId::SubRetry);
            break;
        case HomeKind::NeedsInstall:
            /* Nothing is installed yet, so the version on the right is what the tap would
               install -- saying just "agent 0.11.0" reads like it is already there. */
            second_line = i18n::Format(i18n::StringId::HomeVersionsAvailable,
                                       game_version.c_str(), offer_agent.c_str());
            break;
        case HomeKind::UpdateAvailable:
            second_line = i18n::Format(i18n::StringId::HomeVersionsUpdate, game_version.c_str(),
                                       installed_agent.c_str(), NewerAgentVersion().c_str());
            break;
        default:
            second_line = i18n::Format(i18n::StringId::HomeVersions, game_version.c_str(),
                                       installed_agent.c_str());
            break;
    }

    /* Status block: the dot is centred on the headline, and the versions sit right under it
       (the two lines belong together, so there is no room for a gap).  The version line lines
       up with the headline, not with the dot. */
    const int status_top = kHomeStatusY;
    const int headline_height = m_font.LineHeight(kFontTitle);
    /* The state dot is an indicator, not a control: at 44 px it read like a button.  32 px with
       the text pulled in to match keeps the same optical gap. */
    /* 24 px: clearly an indicator, not a control.  The colour already differs per state --
       teal = ready to install, green = fine, amber = needs attention, red = failed -- see the
       switch above. */
    constexpr int kStatusDot = 24;
    constexpr int kStatusTextX = 44;
    FillRoundedRect(surface, kHomeMargin, status_top + headline_height / 2 - kStatusDot / 2,
                    kStatusDot, kStatusDot, kStatusDot / 2, dot);
    m_font.Draw(surface, kHomeMargin + kStatusTextX, status_top, kFontTitle, kText, Tr(headline));
    m_font.Draw(surface, kHomeMargin + kStatusTextX, status_top + headline_height + 6, kFontBody, kSubtle,
                second_line);

    /* The one message on this screen that is not the install state: an old cheat code left in the
       game directory drives the same in-game input path as the agent, and a missing warning here
       changes what happens in the game.  The warning is about that code, not about the file it
       happens to sit in -- a file full of unrelated entries is not reported at all (see
       env/cheat_file.*) -- but it still names the file by its full path on the card, because that
       is what the player has to open: a bare file name says nothing about where it lives. */
    if (m_report.legacy_cheat_present) {
        const int left = kHomeMargin;
        const int width = surface.width - kHomeMargin * 2;
        constexpr int kBorderWidth = 3;
        constexpr int kRadius = 16;
        FillRoundedRect(surface, left, kHomeWarnY, width, kHomeWarnHeight, kRadius, kWarnBright);
        FillRoundedRect(surface, left + kBorderWidth, kHomeWarnY + kBorderWidth,
                        width - kBorderWidth * 2, kHomeWarnHeight - kBorderWidth * 2,
                        kRadius - kBorderWidth, kWarnTint);
        const int line = m_font.LineHeight(kHomeWarnFont);
        /* No kLabelOpticalBias here: that bias corrects a button label inside a 110 px control, and
           on this strip it pushed the copy visibly below the mark.  The line box is what has to
           line up with the mark; the before/after measurements are in docs/device-acceptance.md. */
        const int text_y = kHomeWarnY + (kHomeWarnHeight - line) / 2;
        DrawWarnMark(surface, left + 38, kHomeWarnY + kHomeWarnHeight / 2, 24, kWarnBright,
                     kWarnTint);
        const char *label = Tr(i18n::StringId::HomeLegacyCheat);
        const int label_width =
            m_font.DrawBold(surface, left + 64, text_y, kHomeWarnFont, kWarn, label);
        m_font.Draw(surface, left + 64 + label_width + 16, text_y, kHomeWarnFont, kWarn,
                    m_report.legacy_cheat_path);
    }

    /* Buttons, with their key badge and the focus ring. */
    const Action *primary = nullptr;
    const Action *check = nullptr;
    const Action *uninstall = nullptr;
    for (const Action &action : m_actions) {
        if (action.id == kActionPrimary) primary = &action;
        if (action.id == kActionCheckUpdate) check = &action;
        if (action.id == kActionUninstall) uninstall = &action;
    }

    auto block = [&](const Action *action, const char *label, const std::string &sub,
                     const char *key, bool filled, bool busy = false) {
        if (action == nullptr) {
            return;
        }
        const Color face = !action->enabled ? kCardDisabled : (filled ? kHeader : kCard);
        const Color border = action->enabled ? (filled ? kHeader : kBorder) : kBorder;
        const Color text = !action->enabled ? kSubtle : (filled ? kOnHeader : kText);
        const int thickness = m_focus == action->id ? 6 : 3;
        FillRoundedRect(surface, action->rect.x, action->rect.y, action->rect.w, action->rect.h,
                        18, border);
        FillRoundedRect(surface, action->rect.x + thickness, action->rect.y + thickness,
                        action->rect.w - thickness * 2, action->rect.h - thickness * 2, 18 - thickness,
                        face);
        /* The key badge is about the physical button, not about the state: it keeps the app's
           accent colour so a state change (purple/orange/red) never repaints it.  Only a
           disabled control greys its badge out, because then the key really does nothing.  While
           a check is in flight the same slot carries the spinner, so the button proves it is
           working and its layout does not move. */
        if (busy) {
            DrawSpinner(surface, action->rect.x + 46, action->rect.y + action->rect.h / 2, 13,
                        filled ? kOnHeader : kHeader);
        } else {
            DrawKeyBadge(surface, m_font, action->rect.x + 46, action->rect.y + action->rect.h / 2,
                         22, key,
                         !action->enabled ? kBorder : (filled ? kOnHeader : kHeader),
                         !action->enabled ? kSubtle : (filled ? kHeader : kOnHeader));
        }
        const int label_width = m_font.Measure(label, kFontHeading + 8);
        const int centre = action->rect.x + action->rect.w / 2;
        m_font.Draw(surface, centre - label_width / 2,
                    action->rect.y + action->rect.h / 2 - 40, kFontHeading + 8, text, label);
        const int sub_width = m_font.Measure(sub, kFontSmall);
        m_font.Draw(surface, centre - sub_width / 2, action->rect.y + action->rect.h / 2 + 8,
                    kFontSmall, filled && action->enabled ? kOnHeader : kSubtle, sub);
    };

    /* One switch per line instead of nesting: the button has to match the state, and the state
       list keeps growing (the "files are gone" case was added last). */
    i18n::StringId label_id = i18n::StringId::BtnUnavailable;
    i18n::StringId sub_id = i18n::StringId::SubInstall;
    switch (m_home_kind) {
        case HomeKind::UpdateAvailable:
            label_id = i18n::StringId::BtnUpdate; /* formatted below: it carries the version */
            break;
        case HomeKind::NeedsInstall: label_id = i18n::StringId::BtnInstall; break;
        case HomeKind::Repair:
        case HomeKind::Incomplete:
            label_id = i18n::StringId::BtnRepair;
            /* The reason lives on the status line, not under the button. */
            sub_id = i18n::StringId::SubInstall;
            break;
        case HomeKind::Failed:
            label_id = i18n::StringId::BtnRetry;
            sub_id = i18n::StringId::SubRetry;
            break;
        case HomeKind::GameMissing:
            label_id = i18n::StringId::BtnRecheck;
            sub_id = i18n::StringId::SubRecheck;
            break;
        case HomeKind::UpToDate:
            label_id = i18n::StringId::BtnUpToDate;
            sub_id = i18n::StringId::SubUpToDate;
            break;
        case HomeKind::Unsupported:
            /* The old subtitle ("the reason is in Details") is gone everywhere: this state now
               has something to do, and the button says what it is. */
            label_id = i18n::StringId::HomeInstallUpdate;
            sub_id = i18n::StringId::HomeInstallUpdateSub;
            break;
    }
    const std::string primary_label =
        m_home_kind == HomeKind::UpdateAvailable
            ? i18n::Format(i18n::StringId::BtnUpdate, NewerAgentVersion().c_str())
            : std::string(Tr(label_id));
    const std::string primary_sub = Tr(sub_id);
    block(primary, primary_label.c_str(), primary_sub, "A", true);
    block(check, Tr(i18n::StringId::BtnCheckUpdate), UpdateSubtitle(), "X", false,
          m_update_running);
    if (uninstall != nullptr) {
        block(uninstall, Tr(i18n::StringId::BtnUninstall), Tr(i18n::StringId::SubUninstall), "Y",
              false);
    }
    if (m_trace_frames > 0) {
        Trace("home: rendered kind=%d actions=%zu focus=%d", static_cast<int>(m_home_kind),
              m_actions.size(), m_focus);
    }
}

void App::RenderDetails(Surface surface) {
    /* The details page is read-only: drop the previous page's controls so a tap on empty space
       cannot fire a button that is not drawn here (its rows, header tabs aside, are not
       buttons). */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    /* The game on this console.  When the manifest knows this title and lists a different version,
       the row carries both: the home screen states "not supported" and points here, so whoever
       answers the question finds what the installed version was compared against.  The second
       version rides along in the existing row (same shape as the agent row above) instead of
       taking a row of its own, which would push the English page down a font size. */
    std::string game = Tr(i18n::StringId::ValueNotAvailable);
    if (!m_report.build.title_id.empty()) {
        const std::string name = "Animal Crossing: New Horizons " + InstalledGameVersion();
        const std::string supported = SupportedGameVersionNote();
        game = (m_gate.status != install::GateStatus::Supported && !supported.empty())
                   ? i18n::Format(i18n::StringId::ValueGameVersionSupported, name.c_str(),
                                  supported.c_str())
                   : name;
    }
    /* The version on the card first -- that is the fact about this console; the commit is a
       development detail and lives in the log.  When this build of the app carries a different
       release (a network update went further than the app), the same row says so, which is the
       one difference a person reading this page needs to understand. */
    std::string agent =
        m_have_state ? m_state.agent_version : std::string(Tr(i18n::StringId::ValueNotInstalled));
    /* Only the release compiled into this build counts as "bundled"; a newer manifest fetched
       from the release host is something to install, not something the app carries. */
    const std::string carried = m_manifest_embedded ? m_manifest.agent.version : std::string();
    if (m_have_state && !carried.empty() && carried != agent) {
        agent = i18n::Format(i18n::StringId::ValueAgentCarried, agent.c_str(), carried.c_str());
    }
    char record[96];
    if (m_have_state) {
        std::snprintf(record, sizeof(record), Tr(i18n::StringId::ValueInstallRecord),
                      static_cast<int>(m_state.files.size()));
    } else {
        std::snprintf(record, sizeof(record), "%s", Tr(i18n::StringId::ValueNoInstallRecord));
    }

    std::vector<Row> version_rows;
    version_rows.push_back({Tr(i18n::StringId::LabelGameVersion), game, kText, 2});
    version_rows.push_back({Tr(i18n::StringId::LabelHos), m_report.hos_version, kText, 1});
    /* Diagnostics: the applet mode and whether the game is open both matter when something
       behaves differently than expected, so they stay available here. */
    /* Plain words instead of the raw enum name: people know "album (applet) mode" (started
       from the album) versus "application (full memory) mode". */
    version_rows.push_back(
        {Tr(i18n::StringId::LabelAppletMode),
         Tr(m_report.applet_type == AppletType_Application ? i18n::StringId::AppletModeApplication
                                                           : i18n::StringId::AppletModeLibrary),
         kText, 1});
    version_rows.push_back({Tr(i18n::StringId::LabelGameRunning),
                            Tr(m_report.application_running ? i18n::StringId::ValuePresent
                                                            : i18n::StringId::ValueAbsent),
                            m_report.application_running ? kWarn : kText, 1});
    version_rows.push_back(
        {Tr(i18n::StringId::LabelContentId),
         m_report.build.content_id.empty() ? Tr(i18n::StringId::ValueNotAvailable)
                                           : m_report.build.content_id,
         m_report.build.content_id.empty() ? kWarn : kGood, 2});
    version_rows.push_back({Tr(i18n::StringId::LabelEmbeddedAgent), agent, kText, 1});
    version_rows.push_back({Tr(i18n::StringId::LabelInstallRecord), record, kText, 1});
    version_rows.push_back({Tr(i18n::StringId::LabelOverrideConfig), m_report.advice.text,
                            m_report.advice.never_applies ? kWarn : kGood, 2});
    version_rows.push_back({Tr(i18n::StringId::LabelLogPath), "/switch/ACNH-Manager/log.txt",
                            kSubtle, 1});
    /* The home screen promises "the reason is in Details", so the gate verdict and any
       detection problem have to be here. */
    version_rows.push_back(
        {Tr(i18n::StringId::LabelGateResult),
         std::string(m_gate.reason) + " [" + PlanActionName(m_plan.action) + "]",
         m_gate.status == install::GateStatus::Supported ? kGood : kWarn, 2});
    std::string problems = m_report.problems;
    if (!m_manifest_error.empty()) {
        problems += (problems.empty() ? "" : "; ") + m_manifest_error;
    }
    if (!m_state_error.empty()) {
        problems += (problems.empty() ? "" : "; ") + m_state_error;
    }
    if (!problems.empty()) {
        version_rows.push_back({Tr(i18n::StringId::LabelProblems), problems, kWarn, 2});
    }
    if (m_report.legacy_cheat_present) {
        version_rows.push_back(
            {Tr(i18n::StringId::LabelLegacyCheat), m_report.legacy_cheat_path, kWarn, 2});
    }

    std::vector<Row> advanced_rows;
    advanced_rows.push_back(
        {Tr(i18n::StringId::LanguageLabel),
         Tr(m_language == i18n::Language::ZhHans ? i18n::StringId::LanguageChinese
                                                 : i18n::StringId::LanguageEnglish),
         kText, 1});
    /* Which manifest the next install would actually use -- not which one happens to sit in
       .rodata: while a verified newer release is adopted, the files come from the network, and
       a row that keeps saying "bundled with the app" is the kind of lie that costs an hour of
       support time. */
    std::string manifest_source;
    if (m_have_update_manifest) {
        manifest_source = i18n::Format(i18n::StringId::ValueNetworkManifest,
                                       ActiveManifest().agent.version.c_str());
    } else if (m_manifest_embedded) {
        manifest_source = std::string(Tr(i18n::StringId::ValueEmbeddedManifest));
    } else {
        manifest_source = m_dev_manifest_path + (m_allow_dev_manifest ? "  [dev allowed]" : "");
    }
    advanced_rows.push_back({Tr(i18n::StringId::LabelManifestSource), manifest_source, kText, 1});
    advanced_rows.push_back({Tr(i18n::StringId::LabelUpdateCheck), UpdateDetailText(),
                             kText, 2});

    /* The details page carries every row we keep; the language switch can make the text longer
       (the English override advice wraps to two lines) and the lower card used to be clipped.
       The tightening order lives in one table instead of a chain of ifs: row gap first, then
       the bottom padding, then the whole page one size down (16 px is the measured minimum that
       still reads right, and smaller type is still readable where a clipped row is not).
       Clipping remains the last resort (docs/architecture.md section 7). */
    struct LayoutTier {
        int row_gap;
        int bottom_pad;
        int font;
    };
    const LayoutTier tiers[] = {
        {kRowGap, kCardPadBottom, kFontBody},
        {0, kCardPadBottom, kFontBody},
        {0, 16, kFontBody},
        {0, 16, kFontSmall}, /* one size down beats cutting the last row off the card */
    };
    /* Row carries its own size, so the smaller step needs its own copies of both lists. */
    std::vector<Row> version_rows_small = version_rows;
    std::vector<Row> advanced_rows_small = advanced_rows;
    for (Row &row : version_rows_small) {
        row.size = kFontSmall;
    }
    for (Row &row : advanced_rows_small) {
        row.size = kFontSmall;
    }
    const int available = page_bottom - page_top - kCardGap;
    LayoutTier tier = tiers[sizeof(tiers) / sizeof(tiers[0]) - 1];
    const std::vector<Row> *version_shown = &version_rows_small;
    const std::vector<Row> *advanced_shown = &advanced_rows_small;
    for (const LayoutTier &candidate : tiers) {
        const bool small = candidate.font == kFontSmall;
        const std::vector<Row> &version_at = small ? version_rows_small : version_rows;
        const std::vector<Row> &advanced_at = small ? advanced_rows_small : advanced_rows;
        const int needed = kCardHeader + RowsHeight(version_at, value_width, candidate.row_gap) +
                           candidate.bottom_pad + kCardHeader +
                           RowsHeight(advanced_at, value_width, candidate.row_gap) +
                           candidate.bottom_pad;
        if (needed <= available) {
            tier = candidate;
            /* Points at the vectors declared above the loop -- not at anything local to this
               iteration. */
            version_shown = small ? &version_rows_small : &version_rows;
            advanced_shown = small ? &advanced_rows_small : &advanced_rows;
            break;
        }
    }
    const int row_gap = tier.row_gap;
    const int bottom_pad = tier.bottom_pad;
    int y = DrawCard(page, page_top, page_bottom, width, value_width,
                     Tr(i18n::StringId::SectionInstall), *version_shown, row_gap, false, bottom_pad);
    const int advanced_top = y;
    DrawCard(page, y, page_bottom, width, value_width, Tr(i18n::StringId::SectionAdvanced),
             *advanced_shown, row_gap, false, bottom_pad);
    /* The language row is a control, so it is tappable like everything else: one row, one
       action, same code path as the A key on this page. */
    const int row_height = m_font.LineHeight(tier.font);
    m_actions.push_back({kActionLanguage,
                         Rect{kMargin, advanced_top + kCardHeader, width, row_height + kRowGap},
                         true, /*focusable=*/true});
    /* The row is a control, so it carries its key badge like every other button. */
    DrawKeyBadge(page, m_font, kMargin + width - 34,
                 advanced_top + kCardHeader + row_height / 2 + 4, 15, "A", kBorder, kText);
}
/* Confirmation pages use the same language as the home screen: what will happen, then one
   real button (plus a way back for touch users).  Paths, hashes and the raw plan reason stay
   in Details -- the person pressing the button only needs the outcome. */
void App::BuildConfirmActions(int width, int height, bool uninstall) {
    (void)uninstall;
    const int inner = width - kHomeMargin * 2;
    const int half = (inner - kHomeGap) / 2;
    const int top = height - kFooterHeight - kCardGap - kHomeSecondaryHeight;
    m_actions.clear();
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, top, half, kHomeSecondaryHeight}, true});
    m_actions.push_back({kActionBack, Rect{kHomeMargin + half + kHomeGap, top, half,
                                           kHomeSecondaryHeight},
                         true});
}

void App::RenderConfirm(Surface surface, bool uninstall) {
    BuildConfirmActions(surface.width, surface.height, uninstall);
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::string action;
    if (uninstall) {
        action = std::string(Tr(i18n::StringId::BtnUninstall));
    } else if (m_plan.action == install::PlanAction::Repair) {
        action = std::string(Tr(i18n::StringId::BtnRepair));
    } else if (!NewerAgentVersion().empty()) {
        action = i18n::Format(i18n::StringId::BtnUpdate, NewerAgentVersion().c_str());
    } else {
        action = std::string(Tr(i18n::StringId::BtnInstall));
    }

    std::vector<Row> rows;
    rows.push_back({nullptr, action, kText, 1, kFontTitle});
    /* What will happen first, then what it means afterwards -- the note is the consequence,
       so it reads wrong above the line it follows from. */
    if (!uninstall) {
        rows.push_back({nullptr,
                        i18n::Format(i18n::StringId::ConfirmWillInstall,
                                     ActiveManifest().agent.version.c_str()),
                        kSubtle, 1});
    } else {
        rows.push_back({nullptr, Tr(i18n::StringId::SubUninstall), kSubtle, 1});
    }
    rows.push_back({nullptr, Tr(uninstall ? i18n::StringId::ConfirmUninstallNote
                                          : i18n::StringId::ConfirmInstallNote),
                    kSubtle, 2});
    const int body_bottom = page_bottom - kHomeSecondaryHeight - kCardGap;
    /* No card title here: the big action line already says "install the agent" / "remove the
       agent", so a title line above it was pure repetition. */
    int y = DrawCard(page, page_top, body_bottom, width, value_width, nullptr, rows, kRowGap,
                     false, kCardPadBottom);

    /* The file list stays: people asked for it, and it is the honest answer to "what is this
       going to write?".  Paths and hashes are fine here -- this is the page they are choosing
       to look at, unlike the home screen. */
    const int list_height = std::max(0, body_bottom - y);
    if (list_height > kCardHeader) {
        /* The uninstall page must not say "files to write": nothing is being written. */
        Card(page, kMargin, y, width, list_height,
             Tr(uninstall ? i18n::StringId::LabelPlanRemove : i18n::StringId::LabelPlan));
        const Surface list = page.Subview(kMargin, y + kCardHeader, width, list_height - kCardHeader);
        /* Entries are laid out from the data, not from what happens to fit: the fixed 52 px row
           used to cut the third file off without a word.  Many files tighten the rows first and
           end with an explicit "and N more" line as the last resort. */
        /* One line per file: "name -> path   size".  The hash lives in state.json and on the
           details page; here it was noise nobody reads. */
        std::vector<std::string> entries;
        if (!uninstall && m_plan.game != nullptr) {
            for (const auto &file : m_plan.game->files) {
                entries.push_back(file.name + "  →  " + file.target + "    " +
                                  FormatSize(file.size));
            }
        } else if (m_have_state) {
            for (const auto &file : m_state.files) {
                /* Same "name -> path" shape as the install page; the record only stores the
                   target, so the name is its last segment. */
                const std::size_t slash = file.target.find_last_of('/');
                const std::string name =
                    slash == std::string::npos ? file.target : file.target.substr(slash + 1);
                entries.push_back(name + "  →  " + file.target + "    " +
                                  FormatSize(file.size));
            }
        }
        if (entries.empty()) {
            m_font.Draw(list, kCardPadX, 0, kFontBody, kWarn, Tr(i18n::StringId::ValueNone),
                        list.width - kCardPadX * 2);
        } else {
            const int count = static_cast<int>(entries.size());
            /* Single-line rows: tight between each other, with a clear gap under the card's
               title (the first row starts below kListTopGap). */
            int entry_height = 32;
            while (entry_height > 24 && count * entry_height + kCardPadBottom > list.height) {
                entry_height -= 4;
            }
            int visible = count;
            while (visible > 1 &&
                   visible * entry_height + kCardPadBottom + kListTopGap > list.height) {
                --visible;
            }
            const bool clipped = visible < count;
            const int rows_space =
                clipped ? list.height - entry_height : list.height; /* room for the note */
            while (clipped && visible * entry_height + kCardPadBottom + kListTopGap > rows_space) {
                --visible;
            }
            int row = kListTopGap;
            for (int i = 0; i < visible; ++i) {
                m_font.Draw(list, kCardPadX, row, kFontSmall, kText, entries[i],
                            list.width - kCardPadX * 2);
                row += entry_height;
            }
            if (clipped) {
                m_font.Draw(list, kCardPadX, row + 4, kFontSmall, kSubtle,
                            i18n::Format(i18n::StringId::LabelPlanMore, count - visible).c_str(),
                            list.width - kCardPadX * 2);
            }
        }
    }

    /* The two buttons, drawn exactly like the home screen's. */
    for (const Action &action_item : m_actions) {
        const bool primary = action_item.id == kActionPrimary;
        const Color face = primary ? kHeader : kCard;
        const Color border = primary ? kHeader : kBorder;
        const Color text = primary ? kOnHeader : kText;
        const int thickness = m_focus == action_item.id ? 6 : 3;
        FillRoundedRect(surface, action_item.rect.x, action_item.rect.y, action_item.rect.w,
                        action_item.rect.h, 18, border);
        FillRoundedRect(surface, action_item.rect.x + thickness, action_item.rect.y + thickness,
                        action_item.rect.w - thickness * 2, action_item.rect.h - thickness * 2,
                        18 - thickness, face);
        const char *key = primary ? "A" : "B";
        DrawKeyBadge(surface, m_font, action_item.rect.x + 46,
                     action_item.rect.y + action_item.rect.h / 2, 22, key,
                     primary ? kOnHeader : dot_face(), primary ? kHeader : kOnHeader);
        const char *label = Tr(primary ? (uninstall ? i18n::StringId::ConfirmRemove
                                                    : i18n::StringId::ConfirmStart)
                                       : i18n::StringId::LabelBack);
        DrawCenteredLabel(surface, m_font, action_item.rect, kFontHeading + 8, text, label);
    }
}

void App::RenderInstall(Surface surface) { RenderConfirm(surface, false); }

void App::RenderUninstall(Surface surface) { RenderConfirm(surface, true); }

/* The progress page exists because the install/uninstall engine runs on the frame-loop thread:
   nothing can animate while it works, so the honest thing to do is say what is happening.  One
   frame goes up before the first byte moves and one after every file (see BeginProgress /
   UpdateProgress); a player watching a network download therefore sees "Installing the agent /
   subsdk9 (1/3)" instead of a frozen confirmation page. */
void App::BeginProgress(bool uninstall, int total) {
    m_progress_uninstall = uninstall;
    m_progress_step.clear();
    m_progress_index = 0;
    m_progress_total = total;
    m_page = Page::Progress;
    Render();
}

/* Called from the engine's callback: same thread as the frame loop, between two files, so
   nothing else is drawing or reading the card at that moment. */
void App::UpdateProgress(const install::Progress &progress) {
    m_progress_step = progress.step;
    m_progress_index = progress.index;
    m_progress_total = progress.total;
    Render();
}

/* Paints the recovered code as squares: one filled rect per black module, on a white plate.
   `module` is the on-screen size of one module, so the caller picks how large it lands. */
void DrawQrCode(Surface surface, const ui::QrMatrix &qr, int x, int y, int module) {
    const int side = qr.Size() * module;
    FillRect(surface, x, y, side + module * 2, side + module * 2, Color{0xFF, 0xFF, 0xFF, 0xFF});
    for (int row = 0; row < qr.Size(); ++row) {
        for (int column = 0; column < qr.Size(); ++column) {
            if (qr.At(row, column)) {
                FillRect(surface, x + module + column * module, y + module + row * module, module,
                         module, Color{0x00, 0x00, 0x00, 0xFF});
            }
        }
    }
}

/* The chat-code explainer.  Text first (what a code is, how to send one, where the numbers come
   from), then the mini-app's code with its caption and the guide address for people who would
   rather not install WeChat. */
void App::RenderGuide(Surface surface) {
    /* Read-only page: no controls of its own (the header tabs and the footer come from the
       shared chrome, and both go through the action table). */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({nullptr, Tr(i18n::StringId::GuideTitle), kText, 1, kFontTitle});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideWhat), kText, 2});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideHow), kText, 2});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideHowMore), kText, 2});
    rows.push_back({nullptr, Tr(i18n::StringId::GuideWhere), kText, 2});
    /* Let DrawCard size the card: it adds the top and bottom padding itself and only clamps to
       `bottom`.  Handing it a pre-computed bottom that already included one padding made it
       clamp away the *top* padding, which left the last line sitting on the card's edge. */
    const int text_card_bottom =
        DrawCard(page, page_top, page_bottom, width, value_width, nullptr, rows, kRowGap, false,
                 kCardPadBottom);

    /* The code sits in its own card under the text, with the caption beside it.  Its module size
       is derived from the space that is actually left: a fixed 7 px per module overflowed the
       page once the explanation grew to four lines, and the footer then covered the code's last
       module rows (measured on hardware: ink down to y=657 against a footer starting at ~648). */
    const int card_top = text_card_bottom; /* DrawCard's return value already skips the gap */
    const int qr_pad = 12;
    const int space_for_code = page_bottom - card_top - qr_pad * 2;
    int module = 7; /* 37 modules -> 259 px, the size that reads best on the console screen */
    if (!m_guide_qr.Empty()) {
        const int fits = space_for_code / (m_guide_qr.Size() + 2); /* + the quiet zone */
        module = std::clamp(fits, 4, 7);
    }
    const int qr_side = m_guide_qr.Empty() ? 0 : m_guide_qr.Size() * module;
    const int card_height = std::max(qr_side + qr_pad * 2 + module * 2, 120);
    FillRoundedRect(page, kMargin, card_top, width, card_height, 14, kCard);
    StrokeRect(page, kMargin, card_top, width, card_height, 3, kBorder);

    if (!m_guide_qr.Empty()) {
        const int qr_x = kMargin + kCardPadX + qr_pad / 2;
        const int qr_y = card_top + (card_height - qr_side) / 2 - module;
        DrawQrCode(page, m_guide_qr, qr_x, qr_y, module);
        /* The middle of the code is the white plate (the logo it covered is not part of the
           matrix); put the brand mark there, the way the original code carries its logo.  It
           stays well inside the plate, so what the decoder sees is unchanged. */
        const int mark_size = module * 4;
        const int mark_width = m_font.Measure(Tr(i18n::StringId::GuideMark), mark_size);
        /* Centre the mark on the plate.  The line box is taller than the glyphs and hangs them a
           little high: measured on hardware the ink landed 4 px above the code's centre, hence
           the downward bias. */
        constexpr int kMarkBiasY = 5;
        const int qr_centre_x = qr_x + module + qr_side / 2;
        const int qr_centre_y = qr_y + module + qr_side / 2;
        m_font.Draw(page, qr_centre_x - mark_width / 2,
                    qr_centre_y - m_font.LineHeight(mark_size) / 2 + kMarkBiasY, mark_size, kText,
                    Tr(i18n::StringId::GuideMark));
        const int text_x = qr_x + qr_side + module * 2 + kCardPadX;
        const int text_width = kMargin + width - kCardPadX - text_x;
        const int text_y = card_top + (card_height - m_font.LineHeight(kFontBody)) / 2;
        m_font.Draw(page, text_x, text_y, kFontBody, kText,
                    m_font.Fit(Tr(i18n::StringId::GuideScan), kFontBody, text_width, 2),
                    text_width);
    } else {
        /* No code in this build: say where the numbers come from instead of leaving a hole. */
        m_font.Draw(page, kMargin + kCardPadX, card_top + 26, kFontBody, kText,
                    m_font.Fit(Tr(i18n::StringId::GuideWhere), kFontBody, value_width, 3),
                    value_width);
    }
}

/* One place for "open the chat-code explainer, or leave it": the + key and the header's "+"
   entry both come through here, so they cannot drift apart. */
void App::ToggleGuide() {
    if (m_page == Page::Guide) {
        m_page = m_guide_return;
    } else {
        m_guide_return = m_page;
        m_page = Page::Guide;
        Trace("guide: opened from page %d", static_cast<int>(m_guide_return));
    }
    m_focus = FirstEnabled(m_actions);
}


/* The game installer: the app's one modal page.  Entering it remembers the page we came from
   (B returns there) and, while it is up, the header hides the four entries and shows the storage
   bars instead -- the page is exclusive, so offering a way out that is not B would be a lie. */

/* True when `title_id` belongs to a title this build supports.  The manifest lists the *base*
   id while a package's CNMT carries the patch id (base + 0x800) or a DLC id (base + 0x1000), so
   comparing the strings would say "not ours" for our own game's update.  The family is the high
   48 bits -- the same rule the running-game check uses. */

/* True when this build's agent supports `version` of `title_id`: same title family, and the
   version the manifest lists -- the gate compares versions exactly, so a 2.0.0 update of our own
   game is still not something the agent can attach to. */
bool App::AgentSupportedFor(const std::string &title_id, std::uint32_t version) const {
    const unsigned long long wanted = std::strtoull(title_id.c_str(), nullptr, 16);
    if (wanted == 0) {
        return false;
    }
    for (const auto &game : ActiveManifest().games) {
        const unsigned long long known = std::strtoull(game.title_id.c_str(), nullptr, 16);
        if (known != 0 && (known >> 16) == (wanted >> 16) && game.version == version) {
            return true;
        }
    }
    return false;
}

std::string App::AgentSupportedVersionsFor(const std::string &title_id) const {
    const unsigned long long wanted = std::strtoull(title_id.c_str(), nullptr, 16);
    if (wanted == 0) {
        return "";
    }
    std::string joined;
    for (const auto &game : ActiveManifest().games) {
        const unsigned long long known = std::strtoull(game.title_id.c_str(), nullptr, 16);
        if (known == 0 || (known >> 16) != (wanted >> 16)) {
            continue;
        }
        if (game.display_version.empty()) {
            continue;
        }
        if (joined.find(game.display_version) != std::string::npos) {
            continue; /* the manifest may list the same game twice (profiles) */
        }
        if (!joined.empty()) {
            joined += " / ";
        }
        joined += game.display_version;
    }
    return joined;
}

bool App::TitleIsOurs(const std::string &title_id) const {
    const unsigned long long wanted = std::strtoull(title_id.c_str(), nullptr, 16);
    if (wanted == 0) {
        return false;
    }
    for (const auto &game : ActiveManifest().games) {
        const unsigned long long known = std::strtoull(game.title_id.c_str(), nullptr, 16);
        if (known != 0 && (known >> 16) == (wanted >> 16)) {
            return true;
        }
    }
    return false;
}

void App::OpenGameInstall() {
    if (m_page == Page::GameInstall) {
        return;
    }
    m_game_install_return = m_page;
    m_page = Page::GameInstall;
    m_install_screen = InstallScreen::Choose; /* always start at "pick a source" */
    m_file_dir = "/";
    m_focus = FirstEnabled(m_actions);
    Trace("install: opened from page %d", static_cast<int>(m_game_install_return));
}

/* The SD card as a flat list of what the player can pick: ".." to go up, then directories, then
   the files this app knows how to install.  Anything else is shown dimmed rather than hidden --
   a file that is simply absent reads as "the app cannot see my card". */
void App::RefreshFileList() {
    m_file_list.SetItems({});
    if (m_sd == nullptr) {
        return;
    }
    std::vector<ui::ListItem> items;
    if (m_file_dir != "/") {
        items.push_back({"..", Tr(i18n::StringId::InstallUpOneLevel), true});
    }
    const util::FsPath dir(m_file_dir);
    FsDir directory{};
    if (R_FAILED(fsFsOpenDirectory(m_sd, dir.c_str(),
                                   FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &directory))) {
        Trace("install: cannot open %s", m_file_dir.c_str());
        m_file_list.SetItems(std::move(items));
        return;
    }
    FsDirectoryEntry entries[64]{};
    s64 total = 0;
    if (R_SUCCEEDED(fsDirRead(&directory, &total, 64, entries))) {
        for (s64 i = 0; i < total && i < 64; ++i) {
            const std::string name = entries[i].name;
            if (entries[i].type == FsDirEntryType_Dir) {
                items.push_back({name + "/", Tr(i18n::StringId::InstallDirectory), true});
            } else if (entries[i].type == FsDirEntryType_File) {
                const bool supported = install::IsSupportedExtension(name);
                items.push_back({name, supported ? std::string(Tr(i18n::StringId::InstallPackage))
                                                 : std::string(Tr(i18n::StringId::InstallIgnored)),
                                 supported});
            }
        }
    }
    fsDirClose(&directory);
    /* The card's own order is whatever the file system feels like; sort so "..", directories and
       then files read the same way twice, which is what makes a picker predictable. */
    std::stable_sort(items.begin() + (m_file_dir == "/" ? 0 : 1), items.end(),
                     [](const ui::ListItem &lhs, const ui::ListItem &rhs) {
                         const bool lhs_dir = !lhs.name.empty() && lhs.name.back() == '/';
                         const bool rhs_dir = !rhs.name.empty() && rhs.name.back() == '/';
                         if (lhs_dir != rhs_dir) {
                             return lhs_dir;
                         }
                         return lhs.name < rhs.name;
                     });
    m_file_list.SetItems(std::move(items));
    Trace("install: %s -> %zu entries", m_file_dir.c_str(), m_file_list.Items().size());
}

/* Two bars where the tabs were: the SD card and the console's own memory, in content-storage
   terms (the same call the space check uses, so the screen and the check cannot disagree).
   A storage that cannot be read shows the placeholder, never a made-up zero. */
void App::RenderStorageBars(Surface surface) {
    struct Bar {
        i18n::StringId label;
        install::TargetStorage target;
    };
    const Bar bars[2] = {{i18n::StringId::StorageSdCard, install::TargetStorage::SdCard},
                         {i18n::StringId::StorageSystem, install::TargetStorage::Nand}};
    constexpr int kBarWidth = 150;
    constexpr int kBarHeight = 8;
    int x_right = surface.width - kMargin;
    for (int i = 1; i >= 0; --i) {
        std::uint64_t free_bytes = 0;
        std::uint64_t total_bytes = 0;
        std::string error;
        const bool ok = install::ContentInstaller::Space(bars[i].target, &free_bytes, &total_bytes,
                                                         &error);
        /* "free / total" in GB with one decimal, the shape the design fixes; a storage that
           cannot be read says so instead of showing 0.0 (design section 8). */
        char caption[64];
        if (ok) {
            std::snprintf(caption, sizeof(caption), "%.1f / %.1f GB",
                          static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0),
                          static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0));
        } else {
            std::snprintf(caption, sizeof(caption), "%s", Tr(i18n::StringId::StorageUnavailable));
        }
        char line[96];
        std::snprintf(line, sizeof(line), "%s  %s", Tr(bars[i].label), caption);
        const int width = m_font.Measure(line, kFontSmall);
        const int x = x_right - width;
        m_font.Draw(surface, x, 22, kFontSmall, kOnHeader, line);
        /* The bar is as wide as the text above it, so the two read as one block; it used to be a
           fixed 150 px, which left the bar visibly shorter than its own caption. */
        const int bar = width > kBarWidth ? width : kBarWidth;
        FillRoundedRect(surface, x, 52, bar, kBarHeight, kBarHeight / 2, kHeaderBadge);
        if (ok && total_bytes > 0) {
            const double used = 1.0 - static_cast<double>(free_bytes) / static_cast<double>(total_bytes);
            const int filled = static_cast<int>(bar * used);
            if (filled > 0) {
                FillRoundedRect(surface, x, 52, filled, kBarHeight, kBarHeight / 2, kOnHeader);
            }
        }
        x_right -= bar + 30;
    }
}


/* The file picker: one card, the directory as rows, the focus as a highlighted row.  Every row's
   hit area comes from the same kRowHeight the list uses, so a tap and the focus can never
   disagree about which row is which. */
/* Going up one level, and picking the focused row: both are "what does the list currently say",
   so they live next to the list instead of in the key handler. */
void App::LeaveDirectory() {
    if (m_file_dir == "/" || m_file_dir.empty()) {
        return;
    }
    std::string parent = m_file_dir;
    while (parent.size() > 1 && parent.back() == '/') {
        parent.pop_back();
    }
    const std::size_t slash = parent.find_last_of('/');
    parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
    if (parent.empty()) {
        parent = "/";
    }
    m_file_dir = parent;
    RefreshFileList();
}

/* A directory opens, an installable file becomes the picked package.  The install itself starts
   from the confirm screen, which is the next screen to be written -- until then the pick is
   recorded and logged so the key map can be tested on hardware. */
void App::PickFocusedFile() {
    const int index = m_file_list.Focus();
    const auto &items = m_file_list.Items();
    if (index < 0 || index >= static_cast<int>(items.size())) {
        return;
    }
    const std::string name = items[static_cast<std::size_t>(index)].name;
    if (name == "..") {
        LeaveDirectory();
        return;
    }
    if (!name.empty() && name.back() == '/') {
        m_file_dir += name.substr(0, name.size() - 1) + "/";
        RefreshFileList();
        return;
    }
    m_picked_file = m_file_dir + name;
    Trace("install: picked %s", m_picked_file.c_str());
    OpenPickedPackage();
}

/* Opens the picked file and parses it, then shows what it is.  Nothing is written yet: the
   confirm screen is the player's chance to say "no, that was the wrong file". */
void App::OpenPickedPackage() {
    m_install_error.clear();
    m_install_status = install::InstallStatus{};
    m_file_source.reset();
    m_game_install.reset();
    if (m_sd == nullptr || m_picked_file.empty()) {
        m_install_error = "no file picked";
        return;
    }
    install::InstallOptions options; /* the reference implementation's defaults (design section 5) */
    m_file_source = std::make_unique<install::LocalFileSource>();
    std::uint64_t size = 0;
    std::string error;
    if (!m_file_source->Open(m_sd, m_picked_file, &size, &error)) {
        /* A file that cannot even be opened is a result, not a silent no-op: the player has to
           be told, and log.txt has to say which file and why. */
        m_install_error = error;
        m_file_source.reset();
        Trace("install: cannot open %s: %s", m_picked_file.c_str(), error.c_str());
        m_install_screen = InstallScreen::Result;
        return;
    }
    m_game_install = std::make_unique<install::GameInstall>(*m_file_source, m_picked_file, size,
                                                           options.target, options);
    if (!m_game_install->Prepare(&error)) {
        m_install_error = error;
        std::string cleanup_error;
        m_game_install->CleanupAfterFailure(&cleanup_error);
        m_game_install.reset();
        m_file_source.reset();
        /* The raw text is the installer's diagnostic (English, technical) and belongs in
           log.txt; the screen gets a line the player can act on.  A running game is not a dead
           end -- closing it and picking the same file again is one step -- so that case goes
           back to the picker instead of to a result page that only offers "back". */
        Trace("install: %s is not installable: %s", m_picked_file.c_str(), error.c_str());
        if (error == "game-running") {
            m_install_screen = InstallScreen::Files;
            return;
        }
        m_install_error = error.find("no installable content") != std::string::npos
                              ? "no-content"
                              : "not-a-package";
        m_install_screen = InstallScreen::Result;
        return;
    }
    m_install_status = m_game_install->Status();
    Trace("install: %s -> %s (%s)", m_picked_file.c_str(),
          m_install_status.stage == install::InstallStage::Skipped ? "skip" : "confirm",
          m_install_status.detail.c_str());
    m_install_screen = m_install_status.stage == install::InstallStage::Skipped
                           ? InstallScreen::Result
                           : InstallScreen::Confirm;
}


/* Runs the whole transfer on this thread.  The engine's steps are bounded (4 MB each), so the
   screen can be redrawn while it works; redrawing every step would spend more time in the
   compositor than in the copy, so a frame every 64 MB is what the player actually needs. */
void App::RunGameInstall() {
    if (m_game_install == nullptr) {
        return;
    }
    m_install_screen = InstallScreen::Progress;
    m_install_error.clear();
    Render();
    std::string error;
    if (!m_game_install->Begin(&error)) {
        m_install_error = error;
        Trace("install: begin failed: %s", error.c_str());
        m_game_install->Abandon(&error);
        m_install_screen = InstallScreen::Result;
        Render();
        return;
    }
    int steps = 0;
    while (m_game_install->Step(&error)) {
        m_install_status = m_game_install->Status();
        if ((++steps % 16) == 0) {
            Render();
        }
    }
    m_install_status = m_game_install->Status();
    if (m_install_status.stage == install::InstallStage::Failed) {
        m_install_error = m_install_status.error;
        /* The failure has to reach log.txt in full: the screen may wrap or clip it, and a
           diagnostic that only exists on a 1280-pixel card is not a diagnostic. */
        Trace("install: transfer failed: %s", m_install_error.c_str());
        m_game_install->Abandon(&error);
        m_install_screen = InstallScreen::Result;
        Render();
        return;
    }
    /* Everything is in: from here the install cannot be cancelled, and the screen says so. */
    m_install_screen = InstallScreen::Progress;
    Render();
    /* The outcome and the stage are logged *before* anything else touches the install: the run
       that reported success while the console kept the old version left no trace at all, which
       meant guessing instead of reading. */
    const bool finished = m_game_install->Finish(&error);
    {
        const auto &status = m_game_install->Status();
        Trace("install: finish returned %d stage=%d%s%s", finished ? 1 : 0,
              static_cast<int>(status.stage), error.empty() ? "" : " error=", error.c_str());
    }
    if (!finished) {
        m_install_error = error;
        Trace("install: finish failed: %s", error.c_str());
        m_game_install->Abandon(&error);
    }
    m_install_status = m_game_install->Status();
    m_install_screen = InstallScreen::Result;
    Trace("install: finished stage=%d%s%s", static_cast<int>(m_install_status.stage),
          m_install_error.empty() ? "" : " error=", m_install_error.c_str());
    m_game_install.reset();
    m_file_source.reset();
    Collect(); /* the home screen's verdict may have changed: the gate watches the game version */
    Render();
}

/* The progress screen: how far along by *bytes*, because one content in a real update is several
   GB and the others are kilobytes -- "3 of 5 contents" would sit at 20% until the very end. */
void App::RenderInstallProgress(Surface surface) {
    m_actions.clear();
    const int card_x = 200;
    const int card_top = kHeaderHeight + 74;
    const int card_width = surface.width - card_x * 2;
    const int card_height = 330;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);

    const bool finalizing = m_install_status.stage == install::InstallStage::Finalizing;
    const char *stage = Tr(finalizing ? i18n::StringId::ProgressFinalize
                                      : i18n::StringId::ProgressReceive);
    const int stage_width = m_font.Measure(stage, kFontHeading);
    m_font.Draw(surface, card_x + card_width / 2 - stage_width / 2, card_top + 28, kFontHeading,
                kText, stage);

    char counter[64];
    std::snprintf(counter, sizeof(counter), Tr(i18n::StringId::ProgressContents),
                  m_install_status.done_contents, m_install_status.total_contents);
    const int counter_width = m_font.Measure(counter, kFontSmall);
    m_font.Draw(surface, card_x + card_width / 2 - counter_width / 2, card_top + 70, kFontSmall,
                kSubtle, counter);

    const int name_width = m_font.Measure(m_install_status.current_name.c_str(), kFontBody);
    m_font.Draw(surface, card_x + card_width / 2 - name_width / 2, card_top + 104, kFontBody,
                kText, m_install_status.current_name.c_str());

    const int bar_x = card_x + 60;
    const int bar_y = card_top + 182;
    const int bar_width = card_width - 200;
    constexpr int kBarHeight = 16;
    FillRoundedRect(surface, bar_x, bar_y, bar_width, kBarHeight, kBarHeight / 2, kCardDisabled);
    std::uint64_t total = m_install_status.total_bytes;
    std::uint64_t done = m_install_status.done_bytes;
    if (total > 0) {
        const double ratio = static_cast<double>(done) / static_cast<double>(total);
        const int filled = static_cast<int>(bar_width * (ratio > 1.0 ? 1.0 : ratio));
        if (filled > 0) {
            FillRoundedRect(surface, bar_x, bar_y, filled, kBarHeight, kBarHeight / 2, kHeader);
        }
    }
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%",
                  total > 0 ? static_cast<int>(done * 100 / total) : 0);
    m_font.DrawBold(surface, bar_x + bar_width + 18, bar_y - 10, kFontHeading, kText, percent);

    char detail[96];
    std::snprintf(detail, sizeof(detail), "%.2f / %.2f GB",
                  static_cast<double>(done) / (1024.0 * 1024.0 * 1024.0),
                  static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0));
    const int detail_width = m_font.Measure(detail, kFontSmall);
    m_font.Draw(surface, card_x + card_width / 2 - detail_width / 2, card_top + 222, kFontSmall,
                kSubtle, detail);
    if (finalizing) {
        const char *note = Tr(i18n::StringId::ProgressFinalize);
        const int note_width = m_font.Measure(note, kFontSmall);
        m_font.Draw(surface, card_x + card_width / 2 - note_width / 2, card_top + 256, kFontSmall,
                    kWarn, note);
    }
}


/* The installer's own page.  Which of its screens shows is one variable, so the dispatcher is a
   switch and each screen draws itself; nothing else in the app is allowed to draw while this page
   is up (see AddChromeActions), which is what makes "install is exclusive" true on screen too. */
void App::RenderGameInstall(Surface surface) {
    if (m_install_screen == InstallScreen::Files) {
        RenderInstallFiles(surface);
        return;
    }
    if (m_install_screen == InstallScreen::Confirm) {
        RenderInstallConfirm(surface);
        return;
    }
    switch (m_install_screen) {
        case InstallScreen::Progress: RenderInstallProgress(surface); return;
        case InstallScreen::Result: RenderInstallResult(surface); return;
        case InstallScreen::Choose:
        case InstallScreen::Files:
        case InstallScreen::Confirm: break;
    }

    /* Pick a source.  This screen shows nothing but the three entries and what formats are
       accepted: everything after it follows from that choice. */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    m_font.Draw(surface, kMargin, kHeaderHeight + 24, kFontSmall, kSubtle,
                Tr(i18n::StringId::InstallFormats));

    struct Entry {
        const char *key;
        i18n::StringId label;
        i18n::StringId sub;
        int action;
    };
    const Entry entries[3] = {
        {"Y", i18n::StringId::InstallPickFile, i18n::StringId::InstallPickFileSub,
         kActionInstallLocal},
        {"X", i18n::StringId::InstallDownload, i18n::StringId::InstallDownloadSub,
         kActionInstallDownload},
        {"A", i18n::StringId::InstallUpload, i18n::StringId::InstallUploadSub,
         kActionInstallUpload}};
    constexpr int kCardGap = 24;
    constexpr int kCardHeight = 210;
    const int card_top = kHeaderHeight + 70;
    const int card_width = (width - kCardGap * 2) / 3;
    for (int i = 0; i < 3; ++i) {
        const int x = kMargin + i * (card_width + kCardGap);
        const Rect rect{x, card_top, card_width, kCardHeight};
        FillRoundedRect(surface, rect.x, rect.y, rect.w, rect.h, 18, kBorder);
        FillRoundedRect(surface, rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 15,
                        m_focus == entries[i].action ? kCardDisabled : kCard);
        DrawKeyBadge(surface, m_font, rect.x + 46, rect.y + rect.h / 2, 22, entries[i].key, kHeader,
                     kOnHeader);
        const char *label = Tr(entries[i].label);
        const int label_width = m_font.Measure(label, kFontHeading + 8);
        m_font.Draw(surface, rect.x + rect.w / 2 - label_width / 2, rect.y + rect.h / 2 - 40,
                    kFontHeading + 8, kText, label);
        const char *sub = Tr(entries[i].sub);
        const int sub_width = m_font.Measure(sub, kFontSmall);
        m_font.Draw(surface, rect.x + rect.w / 2 - sub_width / 2, rect.y + rect.h / 2 + 8,
                    kFontSmall, kSubtle, sub);
        m_actions.push_back({entries[i].action, rect, true});
    }
    m_focus = m_actions.empty() ? -1 : 0;
}

/* The result screen: four titles, and three rows that say what it was, what happened and what
   the player can do now.  Nothing here points at another page -- the design keeps the whole
   answer on this screen (section 8). */

/* The file picker: one card, the directory as rows, the focus as a highlighted row with the mark
   on its *lower* edge.  Every row's hit area comes from the same kRowHeight the list uses. */
void App::RenderInstallFiles(Surface surface) {
    m_actions.clear();
    const int card_x = kMargin;
    const int card_top = kHeaderHeight + 24;
    const int card_width = surface.width - kMargin * 2;
    const int card_height = surface.height - card_top - kFooterHeight - 24;
    /* The list leaves room for a one-line reason at the bottom: the close-the-game hint lives
       there, and a clamped row would draw it half off the card. */
    m_file_list.SetBox(card_width, card_height - 60 - 34);
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);

    m_font.Draw(surface, card_x + 24, card_top + 14, kFontHeading, kText,
                Tr(i18n::StringId::InstallPickFile));
    m_font.Draw(surface, card_x + 24 + m_font.Measure(Tr(i18n::StringId::InstallPickFile),
                                                      kFontHeading) + 18,
                card_top + 24, kFontSmall, kSubtle, m_file_dir.c_str());

    if (!m_install_error.empty()) {
        m_font.Draw(surface, card_x + 24, card_top + card_height - 46, kFontSmall, kBad,
                    m_install_error == "game-running" ? Tr(i18n::StringId::InstallGameRunning)
                                                     : m_install_error.c_str());
    }
    const ui::ListWindow window = m_file_list.Window();
    const int list_top = card_top + 60;
    const std::vector<ui::ListItem> &items = m_file_list.Items();
    if (items.empty()) {
        m_font.Draw(surface, card_x + 24, list_top + 8, kFontBody, kSubtle,
                    Tr(i18n::StringId::InstallNoFiles));
        return;
    }
    for (int row = 0; row < window.count; ++row) {
        const int index = window.first + row;
        if (index >= static_cast<int>(items.size())) {
            break;
        }
        const ui::ListItem &item = items[static_cast<std::size_t>(index)];
        const int y = list_top + row * ui::ListView::kRowHeight;
        const Rect hit{card_x + 16, y - 6, card_width - 32, ui::ListView::kRowHeight - 8};
        if (index == m_file_list.Focus()) {
            FillRoundedRect(surface, hit.x, hit.y, hit.w, hit.h, 12, kCardDisabled);
            FillRoundedRect(surface, hit.x, hit.y + hit.h - 3, hit.w, 3, 0, kHeader);
        }
        const Color ink = item.selectable ? kText : kSubtle;
        m_font.Draw(surface, card_x + 34, y, kFontBody, ink, item.name.c_str());
        const int note_width = m_font.Measure(item.note.c_str(), kFontSmall);
        m_font.Draw(surface, card_x + card_width - 34 - note_width, y + 4, kFontSmall, ink,
                    item.note.c_str());
        if (item.selectable) {
            m_actions.push_back({kActionInstallPickRow, hit, true});
        }
    }
}

/* The confirm screen: what this file is, before anything is written. */
void App::RenderInstallConfirm(Surface surface) {
    m_actions.clear();
    const int card_x = 180;
    const int card_top = kHeaderHeight + 40;
    const int card_width = surface.width - card_x * 2;
    const int card_height = 400;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);

    const install::PackageInfo &package = m_game_install->Package();
    m_font.Draw(surface, card_x + 36, card_top + 28, kFontSmall, kSubtle,
                Tr(i18n::StringId::ConfirmStart));
    const std::string title = m_install_status.game_name.empty() ? package.cnmt.title_id
                                                                : m_install_status.game_name;
    m_font.Draw(surface, card_x + 36, card_top + 56, kFontTitle, kText, title.c_str());

    const int row_x = card_x + 36;
    const int value_x = card_x + 296;
    int y = card_top + 136;
    const auto row = [&](i18n::StringId label, const std::string &text) {
        m_font.Draw(surface, row_x, y, kFontBody, kSubtle, Tr(label));
        m_font.Draw(surface, value_x, y, kFontBody, kText, text.c_str());
        y += 46;
    };
    if (!m_install_status.game_version.empty()) {
        row(i18n::StringId::ConfirmVersion, m_install_status.game_version);
    }
    switch (package.cnmt.type) {
        case install::MetaType::Application:
            row(i18n::StringId::ConfirmType, Tr(i18n::StringId::TypeBase));
            break;
        case install::MetaType::Patch:
            row(i18n::StringId::ConfirmType, Tr(i18n::StringId::TypeUpdate));
            break;
        case install::MetaType::AddOnContent:
            row(i18n::StringId::ConfirmType, Tr(i18n::StringId::TypeDlc));
            break;
        default:
            row(i18n::StringId::ConfirmType, install::MetaTypeLabel(package.cnmt.type));
            break;
    }
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f GB",
                      static_cast<double>(install::TotalBytes(m_game_install->Task())) /
                          (1024.0 * 1024.0 * 1024.0));
        row(i18n::StringId::ConfirmSize, buffer);
    }
    row(i18n::StringId::ConfirmTarget, Tr(i18n::StringId::StorageSdCard));

    if (TitleIsOurs(package.cnmt.title_id)) {
        /* Three cases, not two: our game with a supported version is the only one where the agent
           can be installed afterwards.  Our game with a version the agent does not handle has to
           say so; a package that is not ours says nothing at all. */
        const bool supported = AgentSupportedFor(package.cnmt.title_id, package.cnmt.version);
        if (supported) {
            m_font.Draw(surface, row_x, y + 10, kFontSmall, kGood,
                        Tr(i18n::StringId::ConfirmAgentNote));
        } else {
            /* Name the version the agent does handle: "the agent will not work with this" alone
               leaves the player with nothing to do next. */
            char note[160];
            std::snprintf(note, sizeof(note), Tr(i18n::StringId::InstallAgentUnsupported),
                          AgentSupportedVersionsFor(package.cnmt.title_id).c_str());
            m_font.Draw(surface, row_x, y + 10, kFontSmall, kWarn, note);
        }
    }
    m_actions.push_back({kActionInstallStart,
                         Rect{card_x, card_top, card_width, card_height}, true});
}

void App::RenderInstallResult(Surface surface) {
    m_actions.clear();
    if (m_game_install == nullptr) {
        /* The package was dropped after the run (or never parsed): the screen still says what
           happened, in the player's language. */
        const int x = kMargin;
        m_font.Draw(surface, x, kHeaderHeight + 60, kFontTitle, kBad,
                    Tr(i18n::StringId::ResultFailed));
        i18n::StringId reason = i18n::StringId::InstallNotAPackage;
        if (m_install_error == "no-content") {
            reason = i18n::StringId::InstallNoContent;
        } else if (m_install_error == "game-running") {
            reason = i18n::StringId::InstallGameRunning;
        }
        m_font.Draw(surface, x, kHeaderHeight + 140, kFontBody, kSubtle, Tr(reason));
        return;
    }
    const install::InstallStatus &status = m_install_status;
    const install::PackageInfo &package = m_game_install->Package();

    /* One screen, three things: what this is, what happened, and what the player can do now.
       The first version of this page used "what / result / now" labels in a table and read like a
       form; a result the player only looks at once should read like a sentence. */
    const int card_x = 140;
    const int card_top = kHeaderHeight + 40;
    const int card_width = surface.width - card_x * 2;
    const int card_height = 330;
    FillRoundedRect(surface, card_x, card_top, card_width, card_height, 18, kBorder);
    FillRoundedRect(surface, card_x + 3, card_top + 3, card_width - 6, card_height - 6, 15, kCard);

    /* Only `Done` is "installed".  Any other stage reaching this screen means the run stopped
       somewhere -- and calling that "installed" is how a failed finish looked like a success while
       the console still had the old version. */
    i18n::StringId title = i18n::StringId::ResultFailed;
    Color colour = kBad;
    switch (status.stage) {
        case install::InstallStage::Done:
            title = i18n::StringId::ResultInstalled;
            colour = kGood;
            break;
        case install::InstallStage::Skipped:
            /* Decided by the numbers, not by the path that got us here. */
            title = status.installed_version > status.package_version
                        ? i18n::StringId::ResultSkippedNewer
                        : i18n::StringId::ResultSkippedInstalled;
            colour = kGood;
            break;
        default:
            title = i18n::StringId::ResultFailed;
            colour = kBad;
            break;
    }
    m_font.DrawBold(surface, card_x + 40, card_top + 34, kFontTitle, colour, Tr(title));

    /* What this is: name, version, kind -- one line, the way a player reads a package. */
    const std::string name = m_install_status.game_name.empty() ? package.cnmt.title_id
                                                               : m_install_status.game_name;
    const char *kind = install::MetaTypeLabel(package.cnmt.type);
    i18n::StringId kind_label = i18n::StringId::TypeUpdate;
    if (package.cnmt.type == install::MetaType::Application) {
        kind_label = i18n::StringId::TypeBase;
    } else if (package.cnmt.type == install::MetaType::AddOnContent) {
        kind_label = i18n::StringId::TypeDlc;
    }
    (void)kind;
    char what[256];
    if (status.game_version.empty()) {
        std::snprintf(what, sizeof(what), "%s(%s)", name.c_str(), Tr(kind_label));
    } else {
        std::snprintf(what, sizeof(what), "%s %s(%s)", name.c_str(),
                      status.game_version.c_str(), Tr(kind_label));
    }
    m_font.Draw(surface, card_x + 40, card_top + 118, kFontBody + 4, kText, what);

    /* What happened. */
    char happened[256];
    switch (status.stage) {
        case install::InstallStage::Skipped: {
            char installed_version[32];
            std::snprintf(installed_version, sizeof(installed_version), "%u.%u.%u",
                          static_cast<unsigned>((status.installed_version >> 16) & 0xFFFF),
                          static_cast<unsigned>((status.installed_version >> 8) & 0xFF),
                          static_cast<unsigned>(status.installed_version & 0xFF));
            if (status.installed_version > status.package_version) {
                std::snprintf(happened, sizeof(happened),
                              Tr(i18n::StringId::ResultNewerVersionDetail), installed_version);
            } else {
                std::snprintf(happened, sizeof(happened), "%s",
                              Tr(i18n::StringId::ResultSameVersionDetail));
            }
            break;
        }
        default:
            if (status.stage != install::InstallStage::Done) {
                std::snprintf(happened, sizeof(happened), "%s",
                              m_install_error.empty() ? "the install did not finish" : m_install_error.c_str());
                break;
            }
            [[fallthrough]];
        case install::InstallStage::Done:
        {
            char size[32];
            std::snprintf(size, sizeof(size), "%.1f GB",
                          static_cast<double>(status.total_bytes) / (1024.0 * 1024.0 * 1024.0));
            std::snprintf(happened, sizeof(happened), Tr(i18n::StringId::ResultWroteLine),
                          Tr(i18n::StringId::StorageSdCard), status.total_contents, size);
            break;
        }
    }
    /* Wrapped to the card: a diagnostic like "writing into the placeholder failed (0x307202) at
       offset ..." ran off the screen edge when it was drawn as one line. */
    m_font.Draw(surface, card_x + 40, card_top + 176, kFontBody, kSubtle, happened,
                card_width - 80);

    /* What now: only about the agent, and only when it is true. */
    if (status.stage == install::InstallStage::Done ||
        status.stage == install::InstallStage::Skipped) {
        const std::uint32_t effective = status.stage == install::InstallStage::Skipped
                                            ? status.installed_version
                                            : status.package_version;
        if (AgentSupportedFor(package.cnmt.title_id, effective)) {
            m_font.Draw(surface, card_x + 40, card_top + 236, kFontBody, kGood,
                        Tr(i18n::StringId::ResultAgentReady));
        } else if (TitleIsOurs(package.cnmt.title_id)) {
            m_font.Draw(surface, card_x + 40, card_top + 236, kFontBody, kWarn,
                        Tr(i18n::StringId::InstallAgentUnsupported));
        }
    }
    if (status.modified) {
        m_font.Draw(surface, card_x + 40, card_top + card_height - 40, kFontSmall, kWarn,
                    Tr(i18n::StringId::ResultMoreInDetails));
    }
}

void App::RenderProgress(Surface surface) {
    /* Nothing on this page is pressable and no keys are handled while it is up (the engine owns
       the thread), so it carries no controls and no chrome (see AddChromeActions). */
    m_actions.clear();
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({nullptr,
                    Tr(m_progress_uninstall ? i18n::StringId::ProgressUninstalling
                                            : i18n::StringId::ProgressInstalling),
                    kText, 1, kFontTitle});
    rows.push_back({nullptr, Tr(i18n::StringId::ProgressNote), kSubtle, 2});
    if (!m_progress_step.empty()) {
        std::string current = m_progress_step;
        if (m_progress_total > 0) {
            current += " (" + std::to_string(m_progress_index) + "/" +
                       std::to_string(m_progress_total) + ")";
        }
        rows.push_back({Tr(i18n::StringId::LabelProgressFile), current, kText, 1});
    }
    DrawCard(page, page_top, page_bottom, width, value_width, nullptr, rows, kRowGap, false,
             kCardPadBottom);
}

void App::RenderResult(Surface surface) {
    /* One line about what happened, one line of consequence, one button.  Details keep the
       raw error text for us. */
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({nullptr,
                    Tr(m_result_uninstall
                           ? (m_result_ok ? i18n::StringId::ResultUninstallOk
                                          : i18n::StringId::ResultUninstallFailed)
                           : (m_result_ok ? i18n::StringId::ResultSuccess
                                          : i18n::StringId::ResultFailure)),
                    m_result_ok ? kGood : kBad, 1, kFontTitle});
    rows.push_back({nullptr,
                    /* On the result page the files are already in place, so the note speaks
                       about the present ("restart the game") instead of "once installed". */
                    Tr(m_result_ok ? (m_result_uninstall ? i18n::StringId::ConfirmUninstallNote
                                                         : i18n::StringId::ResultReadyNote)
                                   : i18n::StringId::SubRetry),
                    kSubtle, 2});
    if (!m_result_ok && !m_result_error.empty()) {
        rows.push_back({nullptr, m_result_error, kWarn, 2});
        /* This signature once meant "the path buffer sat near the end of a mapping" (fixed in
           util::FsPath); the hint stays as the safety net, because restarting the manager was
           the action that always recovered the session, whatever the cause turned out to be. */
        if (install::IsStaleSessionError(m_result_error)) {
            rows.push_back({nullptr, Tr(i18n::StringId::ResultRelinkHint), kWarn, 2});
        }
    }
    /* Same shape as the confirmation pages: the card sits at the top, the action button stays
       at the bottom.  Centring a three-line card just left a hole at the top, and the card's
       own title only repeated the headline underneath it. */
    const int area_bottom = page_bottom - kHomeSecondaryHeight - kCardGap;
    DrawCard(page, page_top, area_bottom, width, value_width, nullptr, rows, kRowGap, false,
             kCardPadBottom);

    m_actions.clear();
    const int inner = surface.width - kHomeMargin * 2;
    const int top = surface.height - kFooterHeight - kCardGap - kHomeSecondaryHeight;
    /* One action, one full-width button: a half-width button next to empty space reads as a
       missing control. */
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, top, inner, kHomeSecondaryHeight}, true});
    for (const Action &action_item : m_actions) {
        const int thickness = m_focus == action_item.id ? 6 : 3;
        FillRoundedRect(surface, action_item.rect.x, action_item.rect.y, action_item.rect.w,
                        action_item.rect.h, 18, kHeader);
        FillRoundedRect(surface, action_item.rect.x + thickness, action_item.rect.y + thickness,
                        action_item.rect.w - thickness * 2, action_item.rect.h - thickness * 2,
                        18 - thickness, kHeader);
        DrawKeyBadge(surface, m_font, action_item.rect.x + 46,
                     action_item.rect.y + action_item.rect.h / 2, 22, "A", kOnHeader, kHeader);
        const char *label = Tr(i18n::StringId::Done);
        DrawCenteredLabel(surface, m_font, action_item.rect, kFontHeading + 8, kOnHeader, label);
    }
}

}  // namespace acnh_manager::ui
