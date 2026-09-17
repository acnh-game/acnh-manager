#include "app.hpp"

#include <cstdio>
#include <algorithm>
#include <string>

#include "manifest/manifest.hpp"
#include "log.hpp"

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

constexpr int kHeaderHeight = 72;
constexpr int kFooterHeight = 44;
constexpr int kMargin = 40;
constexpr int kCardGap = 18;
/* Card layout: header height, horizontal padding, distance from the body to the bottom
   border, label column width, row gap.  Card height is measured from the content (see
   RowsHeight); when a page does not fit, the row gap tightens first and clipping is the
   last resort. */
constexpr int kCardHeader = 56;
constexpr int kCardPadX = 24;
constexpr int kCardPadBottom = 12;
/* The label column has to fit the longest English label ("Existing override files", about
   255 px), otherwise it would overlap the value. */
constexpr int kLabelColumn = 280;
constexpr int kRowGap = 8;

const char *AppletTypeName(int type) {
    switch (type) {
        case AppletType_Application: return "Application";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_LibraryApplet: return "LibraryApplet";
        default: return "?";
    }
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

std::string FormatBytes(std::uint64_t size) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(size));
    return buf;
}

}  // namespace

bool App::Init(acnh_manager::Log *log, FsFileSystem &sd, std::string *error) {
    m_log = log;
    m_sd = &sd;
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
        const auto parsed = manifest::Parse(payload::EmbeddedManifestJson(), "0.1.0");
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
        if (!install::ReadManifestFile(*m_sd, m_dev_manifest_path, "0.1.0", !m_allow_dev_manifest,
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
    Trace("collect: gate=%s plan=%s", m_gate.reason.c_str(), PlanActionName(m_plan.action));
}

void App::RefreshPlan() {
    if (m_have_manifest) {
        m_gate = install::Evaluate(&m_manifest, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, m_manifest.agent);
    } else {
        m_gate = install::Evaluate(nullptr, m_report.build);
        m_plan = install::Plan(m_gate, m_have_state ? &m_state : nullptr, manifest::AgentInfo{});
    }
}

void App::RunInstall() {
    if (!m_have_manifest || m_plan.action == install::PlanAction::Blocked ||
        m_plan.game == nullptr) {
        m_result_ok = false;
        /* Even when the engine is never reached, the result page has to say whether anything
           was written -- otherwise dry-run would show up as dry_run=no. */
        m_result_files = 0;
        m_result_dry_run = m_dry_run;
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
    const auto result = install::Install(*m_sd, m_manifest, *m_plan.game, source, m_dry_run,
                                        [this](const install::Progress &progress) {
                                            m_progress_step = progress.step;
                                            m_progress_index = progress.index;
                                            m_progress_total = progress.total;
                                        });
    m_result_ok = result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    m_result_dry_run = result.dry_run;
    if (result.ok) {
        Collect();
    }
    m_page = Page::Result;
}

void App::RunUninstall() {
    const auto result = install::Uninstall(*m_sd, m_dry_run, [this](const install::Progress &p) {
        m_progress_step = p.step;
        m_progress_index = p.index;
        m_progress_total = p.total;
    });
    m_result_ok = result.ok;
    m_result_error = result.error;
    m_result_files = result.files_written;
    m_result_dry_run = result.dry_run;
    if (result.ok) {
        Collect();
    }
    m_page = Page::Result;
}

void App::Run() {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u32 down = padGetButtonsDown(&pad);
        if ((down & (HidNpadButton_L | HidNpadButton_R)) != 0) {
            m_page = m_page == Page::Settings ? Page::Status : Page::Settings;
        }
        if ((down & HidNpadButton_B) != 0) {
            if (m_page == Page::Status) {
                break;
            }
            m_page = Page::Status;
        }
        if ((down & HidNpadButton_Plus) != 0 && m_page == Page::Status) {
            break;
        }
        if ((down & HidNpadButton_A) != 0) {
            switch (m_page) {
                case Page::Status: m_page = Page::Install; break;
                case Page::Install: RunInstall(); break;
                case Page::Uninstall: RunUninstall(); break;
                case Page::Result: Collect(); m_page = Page::Status; break;
                case Page::Settings:
                    m_language = m_language == i18n::Language::ZhHans ? i18n::Language::English
                                                                     : i18n::Language::ZhHans;
                    /* The install engine and the network layer build user-visible messages
                       too, so they read the same process-wide language; and the collected
                       report carries sentences built at Collect() time (the override advice),
                       so re-collect to rebuild them in the new language. */
                    i18n::SetLanguage(m_language);
                    Collect();
                    break;
            }
        }
        if ((down & HidNpadButton_X) != 0 && m_page == Page::Status) {
            m_page = Page::Uninstall;
        }
        if ((down & HidNpadButton_Y) != 0 && m_page == Page::Settings) {
            m_allow_dev_manifest = !m_allow_dev_manifest;
            Collect();
        }
        if ((down & HidNpadButton_X) != 0 && m_page == Page::Settings) {
            m_dry_run = !m_dry_run;
        }
        /* Update check: triggered on demand (a silent check at startup needs a worker
           thread; that moves to the background in M5). */
        if ((down & HidNpadButton_Plus) != 0 && m_page == Page::Settings) {
            const auto check = net::CheckForUpdate(net::kDefaultManifestUrl, net::kDefaultCaPath);
            char buf[256];
            if (check.ok) {
                const auto parsed = manifest::Parse(check.manifest_text, "0.1.0");
                std::snprintf(buf, sizeof(buf),
                              Tr(parsed.ok ? i18n::StringId::UpdateCheckOk
                                           : i18n::StringId::UpdateCheckInvalid),
                              parsed.ok ? parsed.manifest.agent.version.c_str()
                                        : parsed.error.c_str());
            } else {
                std::snprintf(buf, sizeof(buf),
                              Tr(check.skipped ? i18n::StringId::UpdateCheckSkipped
                                               : i18n::StringId::UpdateCheckFailed),
                              check.reason.c_str());
            }
            m_update_status = buf;
            Trace("update check: %s", m_update_status.c_str());
        }
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
        case Page::Status: RenderStatus(surface); break;
        case Page::Install: RenderInstall(surface); break;
        case Page::Uninstall: RenderUninstall(surface); break;
        case Page::Result: RenderResult(surface); break;
        case Page::Settings: RenderSettings(surface); break;
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
    m_font.Draw(surface, kMargin, 16, kFontTitle, kOnHeader, Tr(i18n::StringId::AppTitle));
    const i18n::StringId tab_ids[2] = {i18n::StringId::TabStatus, i18n::StringId::TabSettings};
    int x = surface.width - kMargin;
    for (int i = 1; i >= 0; --i) {
        const char *label = Tr(tab_ids[i]);
        const int width = m_font.Measure(label, kFontHeading);
        x -= width;
        const bool active = (i == 0) == (m_page != Page::Settings);
        m_font.Draw(surface, x, 24, kFontHeading,
                    active ? kOnHeader : MakeColor(0xD5, 0xF2, 0xEC), label);
        if (active) {
            FillRect(surface, x, kHeaderHeight - 6, width, 4, kOnHeader);
        }
        x -= 32;
    }
}

void App::RenderFooter(Surface surface) {
    const int y = surface.height - kFooterHeight;
    FillRect(surface, 0, y, surface.width, kFooterHeight, kCard);
    StrokeRect(surface, 0, y, surface.width, 2, 2, kBorder);
    i18n::StringId hint = i18n::StringId::HintControlsStatus;
    if (m_page == Page::Settings) {
        hint = i18n::StringId::HintControlsSettings;
    } else if (m_page == Page::Install || m_page == Page::Uninstall) {
        hint = i18n::StringId::HintControlsConfirm;
    } else if (m_page == Page::Result) {
        hint = i18n::StringId::HintControlsResult;
    }
    m_font.Draw(surface, kMargin, y + 8, kFontSmall, kSubtle, Tr(hint));
    /* The build stamp sits on the right of the footer: one glance says which build is
       running. */
    const int stamp_width = m_font.Measure(kBuildStamp, kFontSmall);
    m_font.Draw(surface, surface.width - kMargin - stamp_width, y + 8, kFontSmall, kSubtle,
                kBuildStamp);
}

void App::Card(Surface surface, int x, int y, int w, int h, const char *title) {
    FillRect(surface, x, y, w, h, kCard);
    StrokeRect(surface, x, y, w, h, 2, kBorder);
    FillRect(surface, x, y, w, 4, kHeader);
    if (title != nullptr) {
        m_font.Draw(surface, x + 18, y + 14, kFontHeading, kText, title);
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
                  const std::vector<Row> &rows, int row_gap, bool fill_rest) {
    int height = kCardHeader + RowsHeight(rows, value_width, row_gap) + kCardPadBottom;
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
    const Surface body = page.Subview(kMargin, y + kCardHeader, width, height - kCardHeader);
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

void App::RenderStatus(Surface surface) {
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    /* Cards are painted inside the "above the footer" clip, so even a card that measures too
       tall cannot smear over the footer. */
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    /* The three cards collect their rows first, then measure, then paint. */
    std::vector<Row> game;
    game.push_back({Tr(i18n::StringId::LabelHos), m_report.hos_version, kText, 1});
    game.push_back({Tr(i18n::StringId::LabelAppletMode), AppletTypeName(m_report.applet_type), kText, 1});
    game.push_back({Tr(i18n::StringId::LabelGameRunning),
                    Tr(m_report.application_running ? i18n::StringId::ValuePresent
                                                    : i18n::StringId::ValueAbsent),
                    m_report.application_running ? kWarn : kText, 1});
    game.push_back({Tr(i18n::StringId::LabelGameVersion), std::to_string(m_report.build.version),
                    kText, 1});

    std::vector<Row> override_rows;
    override_rows.push_back({Tr(i18n::StringId::LabelOverrideConfig), m_report.advice.text,
                             m_report.advice.never_applies ? kWarn : kGood, 2});
    std::string files;
    for (const auto &file : m_report.exefs) {
        if (!files.empty()) {
            files += ", ";
        }
        files += file.name + " (" + FormatBytes(file.size) + ")";
    }
    override_rows.push_back({Tr(i18n::StringId::LabelExistingFiles),
                             files.empty() ? Tr(i18n::StringId::ValueNone) : files, kText, 2});
    if (m_report.legacy_cheat_present) {
        override_rows.push_back({Tr(i18n::StringId::LabelLegacyCheat), m_report.legacy_cheat_name,
                                 kWarn, 2});
    }

    std::vector<Row> manifest_rows;
    manifest_rows.push_back({Tr(i18n::StringId::LabelManifestState),
                             m_have_manifest
                                 ? Tr(m_manifest_embedded ? i18n::StringId::ValueEmbeddedManifest
                                                          : i18n::StringId::ValueDevManifest)
                                 : Tr(i18n::StringId::ManifestNotEmbedded),
                             m_have_manifest ? kGood : kWarn, 2});
    manifest_rows.push_back({Tr(i18n::StringId::LabelContentId),
                             m_report.build.content_id.empty()
                                 ? Tr(i18n::StringId::ValueNotAvailable)
                                 : m_report.build.content_id,
                             m_report.build.content_id.empty() ? kWarn : kGood, 2});
    manifest_rows.push_back({Tr(i18n::StringId::LabelGateResult),
                             std::string(m_gate.reason) + " [" + PlanActionName(m_plan.action) + "]",
                             m_gate.status == install::GateStatus::Supported ? kGood : kWarn, 2});
    {
        std::string problems = m_report.problems;
        if (!m_manifest_error.empty()) {
            problems += (problems.empty() ? "" : "; ") + m_manifest_error;
        }
        if (!m_state_error.empty()) {
            problems += (problems.empty() ? "" : "; ") + m_state_error;
        }
        if (!problems.empty()) {
            /* The raw diagnostic is long and developer-facing: smaller font, two lines, and
               the full text in log.txt. */
            manifest_rows.push_back(
                {Tr(i18n::StringId::LabelProblems), problems, kWarn, 2, kFontSmall});
        }
    }

    struct CardSpec {
        i18n::StringId title;
        const std::vector<Row> *rows;
    };
    const CardSpec cards[] = {
        {i18n::StringId::SectionGame, &game},
        {i18n::StringId::SectionOverride, &override_rows},
        {i18n::StringId::SectionManifest, &manifest_rows},
    };
    const int card_count = static_cast<int>(sizeof(cards) / sizeof(cards[0]));

    /* When the page does not fit, tighten the row gap first; card heights are measured, so
       the content stays complete. */
    auto content_height = [&](int row_gap) {
        int total = (card_count - 1) * kCardGap;
        for (const CardSpec &card : cards) {
            total += kCardHeader + RowsHeight(*card.rows, value_width, row_gap) + kCardPadBottom;
        }
        return total;
    };
    const int available = page_bottom - page_top;
    int row_gap = kRowGap;
    while (row_gap > 0 && content_height(row_gap) > available) {
        row_gap -= 2;
    }
    if (m_trace_frames > 0) {
        Trace("status: layout row_gap=%d needed=%d available=%d", row_gap, content_height(row_gap),
              available);
    }

    int y = page_top;
    for (int i = 0; i < card_count; ++i) {
        y = DrawCard(page, y, page_bottom, width, value_width, Tr(cards[i].title), *cards[i].rows,
                     row_gap, i + 1 == card_count);
    }
}

void App::RenderInstall(Surface surface) {
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int inner_width = width - kCardPadX * 2;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> summary;
    summary.push_back({Tr(i18n::StringId::LabelPlan),
                       std::string(PlanActionName(m_plan.action)) + " — " + m_plan.reason, kText, 2});
    summary.push_back({Tr(i18n::StringId::LabelDryRun),
                       Tr(m_dry_run ? i18n::StringId::ValueDryRunOn
                                    : i18n::StringId::ValueDryRunOff),
                       m_dry_run ? kGood : kWarn, 1});
    summary.push_back({Tr(i18n::StringId::LabelPayloadDir), m_payload_dir, kSubtle, 1});
    const int y = DrawCard(page, page_top, page_bottom, width, value_width,
                           Tr(i18n::StringId::TabConfirm), summary, kRowGap, false);

    const int list_height = std::max(0, page_bottom - y);
    if (list_height <= kCardHeader) {
        return;
    }
    Card(page, kMargin, y, width, list_height, Tr(i18n::StringId::LabelPlan));
    /* Subview coordinates start at 0, and list.height is the body height that is actually
       available (so a page clip cannot make anything land in the wrong place). */
    const Surface list = page.Subview(kMargin, y + kCardHeader, width, list_height - kCardHeader);
    const int body_height = list.height;
    /* The "press A" line at the bottom always takes one row; the file list gets the rest. */
    const int confirm_height = m_font.LineHeight(kFontBody);
    const int list_limit = std::max(0, body_height - confirm_height - 10);
    const int entry_height = 52;
    int row = 0;
    if (m_plan.game != nullptr) {
        for (const auto &file : m_plan.game->files) {
            if (row + entry_height > list_limit) {
                break;
            }
            m_font.Draw(list, kCardPadX, row, kFontSmall, kText, file.name + "  →  " + file.target,
                        inner_width);
            char meta[128];
            std::snprintf(meta, sizeof(meta), "%s  sha256 %s", FormatBytes(file.size).c_str(),
                          file.sha256.substr(0, 16).c_str());
            m_font.Draw(list, kCardPadX, row + 24, kFontSmall, kSubtle, meta, inner_width);
            row += entry_height;
        }
    } else {
        m_font.Draw(list, kCardPadX, row, kFontBody, kWarn,
                    Tr(i18n::StringId::ResultNoManifest), inner_width);
    }
    m_font.Draw(list, kCardPadX, std::max(0, body_height - confirm_height), kFontBody,
                m_result_ok ? kGood : kSubtle, Tr(i18n::StringId::ConfirmInstall));
}

void App::RenderUninstall(Surface surface) {
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int inner_width = width - kCardPadX * 2;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> intro;
    /* The intro sentence spans the full width (no label column); the dry-run switch follows. */
    intro.push_back({nullptr, Tr(i18n::StringId::UninstallIntro), kText, 2});
    intro.push_back({Tr(i18n::StringId::LabelDryRun),
                     Tr(m_dry_run ? i18n::StringId::ValueDryRunOn
                                  : i18n::StringId::ValueDryRunOff),
                     m_dry_run ? kGood : kWarn, 1});
    const int y = DrawCard(page, page_top, page_bottom, width, value_width,
                           Tr(i18n::StringId::TabUninstall), intro, kRowGap, false);

    const int list_height = std::max(0, page_bottom - y);
    if (list_height <= kCardHeader) {
        return;
    }
    Card(page, kMargin, y, width, list_height, Tr(i18n::StringId::LabelExistingFiles));
    const Surface list = page.Subview(kMargin, y + kCardHeader, width, list_height - kCardHeader);
    const int body_height = list.height;
    const int confirm_height = m_font.LineHeight(kFontBody);
    const int list_limit = std::max(0, body_height - confirm_height - 10);
    const int entry_height = 52;
    int row = 0;
    if (!m_have_state) {
        m_font.Draw(list, kCardPadX, row, kFontBody, kWarn, Tr(i18n::StringId::ValueNone),
                    inner_width);
    } else {
        for (const auto &file : m_state.files) {
            if (row + entry_height > list_limit) {
                break;
            }
            m_font.Draw(list, kCardPadX, row, kFontSmall, kText, file.target, inner_width);
            char meta[128];
            std::snprintf(meta, sizeof(meta), "%s  sha256 %s", FormatBytes(file.size).c_str(),
                          file.sha256.substr(0, 16).c_str());
            m_font.Draw(list, kCardPadX, row + 24, kFontSmall, kSubtle, meta, inner_width);
            row += entry_height;
        }
    }
    m_font.Draw(list, kCardPadX, std::max(0, body_height - confirm_height), kFontBody, kSubtle,
                Tr(i18n::StringId::ConfirmUninstall));
}

void App::RenderResult(Surface surface) {
    const int width = surface.width - kMargin * 2;
    const int inner_width = width - kCardPadX * 2;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    char summary[160];
    std::snprintf(summary, sizeof(summary), "files=%d  dry_run=%s", m_result_files,
                  m_result_dry_run ? "yes" : "no");
    std::string progress;
    if (m_progress_total > 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "last step: %s (%d/%d)", m_progress_step.c_str(),
                      m_progress_index, m_progress_total);
        progress = buf;
    }

    /* Height measured from the content: title + summary + failure reason + progress. */
    const int title_height = m_font.LineHeight(kFontTitle);
    const int body_height = title_height + 12 + m_font.LineHeight(kFontBody) +
                            (m_result_error.empty()
                                 ? 0
                                 : 12 + std::max(1, m_font.LineCount(m_result_error, kFontBody,
                                                                    inner_width)) *
                                            m_font.LineHeight(kFontBody)) +
                            (progress.empty() ? 0 : 12 + m_font.LineHeight(kFontSmall));
    const int card_height =
        std::min(kCardHeader + body_height + kCardPadBottom, page_bottom - page_top);
    Card(page, kMargin, page_top, width, card_height, Tr(i18n::StringId::TabResult));
    const Surface body =
        page.Subview(kMargin, page_top + kCardHeader, width, card_height - kCardHeader);

    int y = 0;
    m_font.Draw(body, kCardPadX, y, kFontTitle, m_result_ok ? kGood : kWarn,
                Tr(m_result_ok ? i18n::StringId::ResultSuccess : i18n::StringId::ResultFailure));
    y += title_height + 12;
    m_font.Draw(body, kCardPadX, y, kFontBody, kSubtle, summary, inner_width);
    y += m_font.LineHeight(kFontBody);
    if (!m_result_error.empty()) {
        y += 12;
        m_font.Draw(body, kCardPadX, y, kFontBody, kWarn, m_result_error, inner_width);
        y += std::max(1, m_font.LineCount(m_result_error, kFontBody, inner_width)) *
             m_font.LineHeight(kFontBody);
    }
    if (!progress.empty()) {
        y += 12;
        m_font.Draw(body, kCardPadX, y, kFontSmall, kSubtle, progress, inner_width);
    }
}

void App::RenderSettings(Surface surface) {
    const int width = surface.width - kMargin * 2;
    const int value_width = width - kCardPadX * 2 - kLabelColumn;
    const int page_top = kHeaderHeight + kMargin / 2;
    const int page_bottom = surface.height - kFooterHeight - kCardGap;
    const Surface page = surface.Clipped(0, page_top, surface.width, page_bottom - page_top);

    std::vector<Row> rows;
    rows.push_back({Tr(i18n::StringId::LanguageLabel),
                    Tr(m_language == i18n::Language::ZhHans ? i18n::StringId::LanguageChinese
                                                            : i18n::StringId::LanguageEnglish),
                    kText, 1});
    rows.push_back({Tr(i18n::StringId::LabelDryRun),
                    Tr(m_dry_run ? i18n::StringId::ValueDryRunOn
                                 : i18n::StringId::ValueDryRunOff),
                    m_dry_run ? kGood : kWarn, 1});
    rows.push_back({Tr(i18n::StringId::LabelPayloadDir),
                    m_manifest_embedded ? Tr(i18n::StringId::ValueEmbeddedManifest) : m_payload_dir,
                    kText, 1});
    rows.push_back({Tr(i18n::StringId::LabelManifestSource),
                    m_manifest_embedded
                        ? std::string(Tr(i18n::StringId::ValueEmbeddedManifest)) + " · agent " +
                              m_manifest.agent.version
                        : m_dev_manifest_path + (m_allow_dev_manifest ? "  [dev allowed]" : ""),
                    kText, 1});
    rows.push_back({Tr(i18n::StringId::LabelUpdateCheck),
                    m_update_status.empty() ? Tr(i18n::StringId::ValueNotChecked) : m_update_status,
                    kText, 2});
    DrawCard(page, page_top, page_bottom, width, value_width, Tr(i18n::StringId::TabSettings), rows,
             kRowGap, false);
}

}  // namespace acnh_manager::ui
