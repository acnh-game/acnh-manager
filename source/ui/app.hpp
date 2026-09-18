#pragma once

/* UI: five pages -- status / install confirm / uninstall / result / settings.
   The whole flow can be dry-run on real hardware: the manifest comes from the SD dev
   manifest, the payload from the payload directory, and dry-run (on by default) verifies
   without writing anything. */
#include <switch.h>

#include <cstddef>
#include <string>
#include <vector>

#include "env/detect.hpp"
#include "i18n/strings.hpp"
#include "install/engine.hpp"
#include "install/gate.hpp"
#include "manifest/manifest.hpp"
#include "net/update.hpp"
#include "payload/embedded.hpp"
#include "ui/action.hpp"
#include "ui/font.hpp"
#include "ui/header_tabs.hpp"
#include "ui/home_state.hpp"
#include "ui/settings.hpp"
#include "ui/touch.hpp"

namespace acnh_manager {
class Log; /* log.hpp */
}

namespace acnh_manager::ui {

#ifndef ACNH_BUILD_STAMP
#define ACNH_BUILD_STAMP "unknown"
#endif

/* Build stamp shown in the footer (injected by tools/build.sh). */
inline constexpr const char *kBuildStamp = ACNH_BUILD_STAMP;

class App {
public:
/* On failure the failing step and its return code go into error (for the log/console). */
    bool Init(acnh_manager::Log *log, FsFileSystem &sd, std::string *error);
    void Exit();
/* Main loop: on the status page, B or + returns. */
    void Run();

private:
    /* Home is the new one-screen UI (state + primary action + two secondary ones); Details
       carries everything professional; Install/Uninstall are the confirmation pages and
       Result reports what happened. */
    enum class Page { Home, Details, Install, Uninstall, Result };

    void Collect();
    void RefreshPlan();
    /* Compare the card with the install record (the record alone is not evidence). */
    void VerifyRecordAgainstCard();
    void LoadSettings();
    void SaveSettings();
    void UpdateHomeState();
    /* "0.11.0(c47d2b47)" -- version plus the first bytes of its payload hash, which is what
       tells two builds of the same version apart. */
    std::string BuildLabel(const std::string &version, const std::string &sha256) const;
    std::string InstalledPayloadHash() const;
    std::string ManifestPayloadHash() const;
    void ToggleLanguage();
    void RunUpdateCheck();
    void RunInstall();
    void RunUninstall();

    void Render();
    void RenderHeader(Surface surface);
    void RenderFooter(Surface surface);
    /* Header tabs and the footer hint live on every page; added after the page's own controls
       so they never steal the entry focus (see app.cpp). */
    void AddChromeActions(int surface_width, int surface_height);
    /* True on the pages that carry their own Ⓑ button; the footer then stays quiet.  One
       helper because the painted footer and the registered action must agree. */
    bool HasOwnBackButton() const;
    void RenderHome(Surface surface);
    void RenderDetails(Surface surface);
    void RenderInstall(Surface surface);
    void RenderUninstall(Surface surface);
    /* Confirmation pages share one implementation: action, outcome, two buttons. */
    void BuildConfirmActions(int width, int height, bool uninstall);
    void RenderConfirm(Surface surface, bool uninstall);
    void RenderResult(Surface surface);
    void Card(Surface surface, int x, int y, int w, int h, const char *title);

    /* Home-screen actions, rebuilt every frame from the same rectangles that get painted, so
       touch hit-testing and the painted buttons can never disagree. */
    void BuildHomeActions(int width, int height);
    void ActivateAction(int id);
    void HandleKeys(u32 down);
    void HandleTouch();

    /* One "label + value" row inside a card.  The value wraps at value_width and the row
       height follows the real line count; anything past max_lines is truncated with an
       ellipsis (the full text stays in log.txt), so a long value never collides with the
       next row. */
    struct Row {
        const char *label{nullptr};
        std::string value;
        Color color{};
        int max_lines{1};
        /* Font size: kFontBody for normal rows, kFontSmall for secondary detail such as
           the raw validator diagnostic. */
        int size{kFontBody};
    };
    /* Not const: wrapping uses the glyph cache (Font::Fit / LineCount share Draw's metrics). */
    int RowsHeight(const std::vector<Row> &rows, int value_width, int row_gap);
    void DrawRows(Surface surface, const std::vector<Row> &rows, int value_width, int row_gap);
    /* Measure first, then paint a "title + rows" card; returns the next card's y.
       page is already clipped to "above the footer"; fill_rest makes the last card fill the
       remaining space. */
    int DrawCard(Surface page, int y, int bottom, int width, int value_width, const char *title,
                 const std::vector<Row> &rows, int row_gap, bool fill_rest,
                 int bottom_pad);

    const char *Tr(i18n::StringId id) const { return i18n::Text(id, m_language); }
    void Trace(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    acnh_manager::Log *m_log{nullptr};
    FsFileSystem *m_sd{nullptr};
    Font m_font{};
    Framebuffer m_fb{};
    bool m_fb_ready{false};
    Page m_page{Page::Home};
    i18n::Language m_language{i18n::Language::ZhHans};
    Touch m_touch{};
    /* Touch press tracking: a tap only fires when the finger goes down and comes back up on
       the same action (Sphaira-style), so a drag never triggers a button by accident. */
    TapTracker m_tap{};
    std::vector<Action> m_actions{};
    int m_focus{-1};
    bool m_should_exit{false};

    HomeKind m_home_kind{HomeKind::NeedsInstall};
    bool m_last_failed{false};
    /* The record says installed, but the card no longer matches it. */
    bool m_files_incomplete{false};
    /* Newer agent version found by the update check (empty when there is none). */
    std::string m_newer_agent{};

    env::EnvironmentReport m_report{};
    manifest::Manifest m_manifest{};
    bool m_have_manifest{false};
    /* Whether the manifest came from the embedded release channel (true) or from the SD dev
       manifest (false). */
    bool m_manifest_embedded{false};
    std::string m_manifest_error{};
    install::GateResult m_gate{};
    install::InstallPlan m_plan{};
    install::InstallState m_state{};
    bool m_have_state{false};
    std::string m_state_error{};

    bool m_allow_dev_manifest{false};
    std::string m_payload_dir{"/switch/ACNH-Manager/payload"};
    std::string m_dev_manifest_path{"/switch/ACNH-Manager/dev-manifest.json"};

    std::string m_progress_step{};
    int m_progress_index{0};
    int m_progress_total{0};
    bool m_result_ok{false};
    std::string m_result_error{};
    int m_result_files{0};
    /* Whether the last action was an uninstall (the result page picks its wording). */
    bool m_result_uninstall{false};
    /* An empty string means "not checked yet" (the UI shows the not-checked string). */
    std::string m_update_status{};
    /* Log every drawing stage of the first frame, so a crash points at one of them. */
    int m_trace_frames{1};
    /* First-frame stage pauses (when dev-pause exists): wait for + after each stage. */
    bool m_stage_pause{false};
    void WaitForPlus(const char *stage);
};

}  // namespace acnh_manager::ui
