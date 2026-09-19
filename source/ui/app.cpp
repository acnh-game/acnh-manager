#include "app.hpp"

#include <cstdio>
#include <algorithm>
#include <string>

#include "manifest/manifest.hpp"
#include "log.hpp"
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
constexpr Color kWarn{0xB5, 0x5A, 0x1E, 0xFF};
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
        const bool pause_before_first_frame =
            m_sd != nullptr &&
            R_SUCCEEDED(fsFsOpenFile(m_sd, "/switch/ACNH-Manager/dev-pause", FsOpenMode_Read, &flag));
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
    if (m_have_manifest) {
        m_gate = install::Evaluate(&m_manifest, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, m_manifest.agent);
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
    in.game_found = !m_report.build.title_id.empty() && m_report.build.version != 0;
    in.supported = m_gate.status == install::GateStatus::Supported;
    in.last_failed = m_last_failed;
    in.files_incomplete = m_files_incomplete;
    in.repair_needed = m_plan.action == install::PlanAction::Repair;
    in.fresh_install = m_plan.action == install::PlanAction::Install;
    in.newer_agent = !m_newer_agent.empty();
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
    /* The update-check sentence is one string built by the network layer in the language of the
       moment ("skipped: missing CA file: <path>"), so it cannot be re-rendered here.  Dropping
       it back to "not checked" beats leaving a stale-language message on the details page; the
       real fix -- the network layer returning an id plus arguments -- rides along with the
       embedded CA bundle work. */
    if (!m_update_status.empty()) {
        m_update_status.clear();
        if (m_log != nullptr) {
            m_log->Line("update check status dropped after a language change");
        }
    }
    Collect();
}

/* Update check: triggered on demand (a silent check at startup needs a worker thread, which
   is a later step).  A newer agent version switches the home screen to "update available". */
void App::RunUpdateCheck() {
    const auto check = net::CheckForUpdate(net::kDefaultManifestUrl);
    char buf[256];
    if (check.ok) {
        const auto parsed = manifest::Parse(check.manifest_text, kAppVersion);
        if (parsed.ok) {
            const std::string &found = parsed.manifest.agent.version;
            const std::string &have = m_have_manifest ? m_manifest.agent.version : found;
            m_newer_agent =
                manifest::CompareVersions(found, have) > 0 ? found : std::string();
            std::snprintf(buf, sizeof(buf), Tr(i18n::StringId::UpdateCheckOk), found.c_str());
        } else {
            m_newer_agent.clear();
            std::snprintf(buf, sizeof(buf), Tr(i18n::StringId::UpdateCheckInvalid),
                          parsed.error.c_str());
        }
    } else {
        std::snprintf(buf, sizeof(buf),
                      Tr(check.skipped ? i18n::StringId::UpdateCheckSkipped
                                       : i18n::StringId::UpdateCheckFailed),
                      check.reason.c_str());
    }
    m_update_status = buf;
    Trace("update check: %s", m_update_status.c_str());
    UpdateHomeState();
}

void App::RunInstall() {
    if (!m_have_manifest || m_plan.action == install::PlanAction::Blocked ||
        m_plan.game == nullptr) {
        m_result_ok = false;
        m_last_failed = true;
        /* Even when the engine is never reached, the result page has to say whether anything
           was written -- otherwise dry-run would show up as dry_run=no. */
        m_result_files = 0;
        m_result_error = m_have_manifest ? m_plan.reason : Tr(i18n::StringId::ResultNoManifest);
        m_page = Page::Result;
        return;
    }
    /* Payload matches the manifest's origin: the embedded manifest uses the embedded
       payload, a dev manifest uses the payload directory on the SD card. */
    payload::EmbeddedPayloadSource embedded_source;
    install::SdFolderPayloadSource sd_source(*m_sd, m_payload_dir);
    install::PayloadSource &source =
        m_manifest_embedded ? static_cast<install::PayloadSource &>(embedded_source)
                            : static_cast<install::PayloadSource &>(sd_source);
    /* Development switch: record every fs call the engine makes, so a failure can be read back
       with the pointer and memory state the kernel saw at that moment. */
    if (DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        install::SetFsTraceSink(m_log);
    }
    const auto result = install::Install(*m_sd, m_manifest, *m_plan.game, source, /*dry_run=*/false,
                                        [this](const install::Progress &progress) {
                                            m_progress_step = progress.step;
                                            m_progress_index = progress.index;
                                            m_progress_total = progress.total;
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
        Collect();
    }
    m_page = Page::Result;
}

void App::RunUninstall() {
    if (DevFlagPresent(*m_sd, "/switch/ACNH-Manager/dev-fsprobe")) {
        install::SetFsTraceSink(m_log);
    }
    const auto result = install::Uninstall(*m_sd, /*dry_run=*/false, [this](const install::Progress &p) {
        m_progress_step = p.step;
        m_progress_index = p.index;
        m_progress_total = p.total;
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

/* Keyboard half of the input model.  Home has three actions with dedicated keys (A for the
   primary one, X and Y for the two secondary ones) plus focus movement for the d-pad, the
   details page uses focus and A, and B always means "back" (or exit on Home). */
void App::HandleKeys(u32 down) {
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
        m_page = Page::Home;
        m_focus = FirstEnabled(m_actions);
        return;
    }
    if ((down & HidNpadButton_Plus) != 0) {
        m_should_exit = true;
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
                    break; /* nothing to do; the headline already says why */
                case HomeKind::NeedsInstall:
                case HomeKind::UpdateAvailable:
                case HomeKind::Repair:
                case HomeKind::Incomplete:
                case HomeKind::Failed:
                    m_page = Page::Install;
                    break;
            }
            break;
        case kActionCheckUpdate: RunUpdateCheck(); break;
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
    m_focus = FirstEnabled(m_actions);
    while (appletMainLoop() && !m_should_exit) {
        padUpdate(&pad);
        HandleKeys(padGetButtonsDown(&pad));
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
        case Page::Details: RenderDetails(surface); break;
        case Page::Install: RenderInstall(surface); break;
        case Page::Uninstall: RenderUninstall(surface); break;
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
    struct Tab {
        const char *key;
        i18n::StringId label;
        Page page;
    };
    const Tab tabs[2] = {{"L", i18n::StringId::TabStatus, Page::Home},
                         {"R", i18n::StringId::TabDetails, Page::Details}};
    int label_width[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        label_width[i] = m_font.Measure(Tr(tabs[i].label), kFontHeading);
    }
    const HeaderTabLayout layout = LayoutHeaderTabs(surface.width, label_width);
    for (int i = 0; i < 2; ++i) {
        const bool active = m_page == tabs[i].page ||
                            (i == 0 && m_page != Page::Details);
        const char *label = Tr(tabs[i].label);
        m_font.Draw(surface, layout.label_x[i], 36, kFontHeading,
                    active ? kOnHeader : kHeaderSubtle, label);
        FillRoundedRect(surface, layout.badge_x[i], kHeaderTabTop, kHeaderTabBadge,
                        kHeaderTabBadge, kHeaderTabBadge / 2,
                        active ? kOnHeader : kHeaderBadge);
        /* Centre the letter on both axes: our Draw() treats y as the top of the line box, so
           bias it by half the box to land the glyph in the middle of the circle. */
        const int key_width = m_font.Measure(tabs[i].key, kFontSmall);
        m_font.Draw(surface, layout.badge_x[i] + (kHeaderTabBadge - key_width) / 2,
                    kHeaderTabTop + 10, kFontSmall, active ? kHeader : kOnHeader, tabs[i].key);
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
    int label_width[2] = {m_font.Measure(Tr(i18n::StringId::TabStatus), kFontHeading),
                          m_font.Measure(Tr(i18n::StringId::TabDetails), kFontHeading)};
    const HeaderTabLayout layout = LayoutHeaderTabs(surface_width, label_width);
    m_actions.push_back({kActionTabStatus, layout.hit[0], true});
    m_actions.push_back({kActionTabDetails, layout.hit[1], true});

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
        m_font.Draw(surface, hint.badge_x + (hint.badge - key_width) / 2, hint.badge_y + 6,
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
       there: one key, one place. */
    return m_page == Page::Install || m_page == Page::Uninstall || m_page == Page::Result;
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

/* Key badge: the physical button that also triggers this control, drawn inside it. */
void DrawKeyBadge(Surface surface, Font &font, int cx, int cy, int radius, const char *key,
                  Color face, Color text) {
    FillRoundedRect(surface, cx - radius, cy - radius, radius * 2, radius * 2, radius, face);
    const int width = font.Measure(key, kFontBody);
    font.Draw(surface, cx - width / 2, cy - kFontBody / 2 - 1, kFontBody, text, key);
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
    const bool primary_enabled =
        m_home_kind != HomeKind::UpToDate && m_home_kind != HomeKind::Unsupported;
    m_actions.clear();
    m_actions.push_back(
        {kActionPrimary, Rect{kHomeMargin, kHomePrimaryY, inner, kHomeButtonHeight}, primary_enabled});
    /* Without the uninstall button there is nothing to pair with, and a half-width button next
       to empty space reads as a missing control, so the check button takes the full row. */
    const int check_width = installed ? half : inner;
    m_actions.push_back({kActionCheckUpdate,
                         Rect{kHomeMargin, kHomeSecondaryY, check_width, kHomeSecondaryHeight},
                         true});
    if (installed) {
        m_actions.push_back({kActionUninstall,
                             Rect{kHomeMargin + half + kHomeGap, kHomeSecondaryY, half,
                                  kHomeSecondaryHeight},
                             true});
    }
}

void App::RenderHome(Surface surface) {
    BuildHomeActions(surface.width, surface.height);
    /* Colour carries the state: teal = ready to act, green = fine, orange = needs attention,
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
    std::string game_version = Tr(i18n::StringId::ValueNotAvailable);
    if (m_gate.game != nullptr && !m_gate.game->display_version.empty()) {
        game_version = m_gate.game->display_version;
    } else if (m_report.build.version != 0) {
        game_version = std::to_string(m_report.build.version);
    }
    const std::string have_agent = m_have_manifest
                                       ? m_manifest.agent.version
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
        case HomeKind::Unsupported:
            second_line = Tr(i18n::StringId::StateUnsupportedSub);
            break;
        case HomeKind::Failed:
            second_line = Tr(i18n::StringId::SubRetry);
            break;
        case HomeKind::NeedsInstall:
            /* Nothing is installed yet, so the version on the right is what the tap would
               install -- saying just "agent 0.11.0" reads like it is already there. */
            second_line = i18n::Format(i18n::StringId::HomeVersionsAvailable,
                                       game_version.c_str(), have_agent.c_str());
            break;
        case HomeKind::UpdateAvailable:
            second_line = i18n::Format(i18n::StringId::HomeVersionsUpdate, game_version.c_str(),
                                       have_agent.c_str(), m_newer_agent.c_str());
            break;
        default:
            second_line =
                i18n::Format(i18n::StringId::HomeVersions, game_version.c_str(), have_agent.c_str());
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
       teal = ready to install, green = fine, orange = needs attention, red = failed -- see the
       switch above. */
    constexpr int kStatusDot = 24;
    constexpr int kStatusTextX = 44;
    FillRoundedRect(surface, kHomeMargin, status_top + headline_height / 2 - kStatusDot / 2,
                    kStatusDot, kStatusDot, kStatusDot / 2, dot);
    m_font.Draw(surface, kHomeMargin + kStatusTextX, status_top, kFontTitle, kText, Tr(headline));
    m_font.Draw(surface, kHomeMargin + kStatusTextX, status_top + headline_height + 6, kFontBody, kSubtle,
                second_line);

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
                     const char *key, bool filled) {
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
           disabled control greys its badge out, because then the key really does nothing. */
        DrawKeyBadge(surface, m_font, action->rect.x + 46, action->rect.y + action->rect.h / 2, 22,
                     key,
                     !action->enabled ? kBorder : (filled ? kOnHeader : kHeader),
                     !action->enabled ? kSubtle : (filled ? kHeader : kOnHeader));
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
            sub_id = i18n::StringId::StateUnsupportedSub;
            break;
    }
    const std::string primary_label =
        m_home_kind == HomeKind::UpdateAvailable
            ? i18n::Format(i18n::StringId::BtnUpdate, m_newer_agent.c_str())
            : std::string(Tr(label_id));
    const std::string primary_sub = Tr(sub_id);
    block(primary, primary_label.c_str(), primary_sub, "A", true);
    block(check, Tr(i18n::StringId::BtnCheckUpdate), Tr(i18n::StringId::SubCheckUpdate), "X", false);
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

    std::string game = Tr(i18n::StringId::ValueNotAvailable);
    if (m_gate.game != nullptr && !m_gate.game->display_version.empty()) {
        game = "Animal Crossing: New Horizons " + m_gate.game->display_version;
    } else if (m_report.build.version != 0) {
        game = std::to_string(m_report.build.version);
    }
    /* Just the version: the commit is a development detail and lives in the log. */
    const std::string agent = m_have_manifest ? m_manifest.agent.version
                                              : std::string(Tr(i18n::StringId::ValueNotInstalled));
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
            {Tr(i18n::StringId::LabelLegacyCheat), m_report.legacy_cheat_name, kWarn, 2});
    }

    std::vector<Row> advanced_rows;
    advanced_rows.push_back(
        {Tr(i18n::StringId::LanguageLabel),
         Tr(m_language == i18n::Language::ZhHans ? i18n::StringId::LanguageChinese
                                                 : i18n::StringId::LanguageEnglish),
         kText, 1});
    advanced_rows.push_back({Tr(i18n::StringId::LabelManifestSource),
                             m_manifest_embedded
                                 ? std::string(Tr(i18n::StringId::ValueEmbeddedManifest))
                                 : m_dev_manifest_path +
                                       (m_allow_dev_manifest ? "  [dev allowed]" : ""),
                             kText, 1});
    advanced_rows.push_back({Tr(i18n::StringId::LabelUpdateCheck),
                             m_update_status.empty() ? Tr(i18n::StringId::ValueNotChecked)
                                                     : m_update_status,
                             kText, 2});

    /* The details page carries every row we keep; the language switch can make the text longer
       (the English override advice wraps to two lines) and the lower card used to be clipped.
       The tightening order lives in one table instead of a chain of ifs: row gap first, then
       the bottom padding, and 16 px is the measured minimum that still reads right.  Clipping
       remains the last resort (docs/architecture.md section 7). */
    struct LayoutTier {
        int row_gap;
        int bottom_pad;
    };
    const LayoutTier tiers[] = {{kRowGap, kCardPadBottom}, {0, kCardPadBottom}, {0, 16}};
    const int available = page_bottom - page_top - kCardGap;
    LayoutTier tier = tiers[sizeof(tiers) / sizeof(tiers[0]) - 1];
    for (const LayoutTier &candidate : tiers) {
        const int needed = kCardHeader + RowsHeight(version_rows, value_width, candidate.row_gap) +
                           candidate.bottom_pad + kCardHeader +
                           RowsHeight(advanced_rows, value_width, candidate.row_gap) +
                           candidate.bottom_pad;
        if (needed <= available) {
            tier = candidate;
            break;
        }
    }
    const int row_gap = tier.row_gap;
    const int bottom_pad = tier.bottom_pad;
    int y = DrawCard(page, page_top, page_bottom, width, value_width,
                     Tr(i18n::StringId::SectionInstall), version_rows, row_gap, false, bottom_pad);
    const int advanced_top = y;
    DrawCard(page, y, page_bottom, width, value_width, Tr(i18n::StringId::SectionAdvanced),
             advanced_rows, row_gap, false, bottom_pad);
    /* The language row is a control, so it is tappable like everything else: one row, one
       action, same code path as the A key on this page. */
    const int row_height = m_font.LineHeight(kFontBody);
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
    } else if (!m_newer_agent.empty()) {
        action = i18n::Format(i18n::StringId::BtnUpdate, m_newer_agent.c_str());
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
                                     m_manifest.agent.version.c_str()),
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
