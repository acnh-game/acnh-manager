#pragma once

/* UI: five pages -- status / install confirm / uninstall / result / settings.
   The whole flow can be dry-run on real hardware: the manifest comes from the SD dev
   manifest, the payload from the payload directory, and dry-run (on by default) verifies
   without writing anything. */
#include <switch.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "env/detect.hpp"
#include "env/forwarder.hpp"
#include "env/startup_runtime.hpp"
#include "i18n/strings.hpp"
#include "install/engine.hpp"
#include "install/gate.hpp"
#include "manifest/manifest.hpp"
#include "net/update.hpp"
#include "net/update_task.hpp"
#include "net/http_server.hpp"
#include "payload/embedded.hpp"
#include "ui/guide_qr.hpp"
#include "ui/action.hpp"
#include "ui/font.hpp"
#include "ui/header_tabs.hpp"
#include "ui/home_state.hpp"
#include "ui/icon_image.hpp"
#include "ui/list_view.hpp"
#include "ui/settings.hpp"
#include "ui/storage_bars.hpp"
#include "util/qr_code.hpp"
#include <memory>

#include "install/game_install.hpp"
#include "install/source_http_client.hpp"
#include "install/source_local_file.hpp"
#include "install/source_http_upload.hpp"
#include "install/source_memory.hpp"
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
    /* The startup check runs in main before the UI initialises; a page is shown only when it has
       something to repair (or a failure to report). */
    void SetStartupPlan(env::StartupPlan plan);
    /* True when the startup page repaired the layout and set the loader's next NRO; main then
       returns to hbl instead of terminating the applet so that load can happen. */
    bool StartupRestartRequested() const { return m_startup_restart; }
/* Main loop: on the status page, B or + returns. */
    void Run();

private:
    /* Home is the new one-screen UI (state + primary action + two secondary ones); Details
       carries everything professional; Install/Uninstall are the confirmation pages and
       Result reports what happened. */
    /* Progress is shown while the engine works: the install runs on this thread, so this page
       is drawn once before the first byte moves and again after every file. */
    enum class Page {
        Home,
        Details,
        Install,
        Uninstall,
        Progress,
        Guide,
        Result,
        GameInstall,
        StartupCheck,
    };

    void Collect();
    /* Writes the environment block into log.txt: what this console is, what game/version is
       installed, which override files exist, and which manifest this build would install from.
       It is written off the one environment collection the app does (the launcher used to collect
       the same thing a second time before starting the UI: about 1.6 s of the launch on hardware,
       measured 2026-10-16), and from the second loop iteration rather than during Init -- each log
       line is one open/write/flush/close per sink, so the block costs about 2.2 s, and it used to
       sit between the framebuffer and the first frame, which is where most of the black screen
       came from. */
    void TraceEnvironment();
    void RefreshPlan();
    /* Compare the card with the install record (the record alone is not evidence). */
    void VerifyRecordAgainstCard();
    void LoadSettings();
    void SaveSettings();
    void UpdateHomeState();
    /* The installed game's version as a person reads it: the console's NACP display string
       ("1.0.0"), the entry this build matched, or the numeric title version. */
    std::string InstalledGameVersion() const;
    /* The game version this build of the app can install (from the manifest entry for the detected
       title) -- but only when it differs from the installed one, because that is the only case
       where naming it explains anything.  Empty when no manifest lists this title, or when both
       sides carry the same version and the gate refused for another reason. */
    std::string SupportedGameVersionNote() const;
    /* Player-facing wording for the gate row on Details.  The raw gate reason stays in the log;
       version mismatches use display versions (for example 2.0.0 / 3.0.3), not title versions. */
    std::string GateResultText() const;
    /* "0.11.0(c47d2b47)" -- version plus the first bytes of its payload hash, which is what
       tells two builds of the same version apart. */
    std::string BuildLabel(const std::string &version, const std::string &sha256) const;
    std::string InstalledPayloadHash() const;
    std::string ManifestPayloadHash() const;
    void ToggleLanguage();
    /* Update check: started from Home's focused check button or once at startup; runs on a
       worker thread, so the UI polls it every frame instead of waiting for the network. */
    void StartUpdateCheck(bool silent = false);
    void PollUpdateCheck();
    void ApplyUpdateCheck(net::UpdateCheckResult result);
    /* Home page: the check button's second line (idle / running / result, in the current
       language, built at render time so a language switch cannot leave a stale sentence). */
    std::string UpdateSubtitle() const;
    /* Details page: the raw outcome, for when someone needs to know what actually happened. */
    std::string UpdateDetailText() const;
    /* The agent version a press of the primary button would install, when that is newer than
       what is installed; empty when there is nothing newer to offer. */
    std::string NewerAgentVersion() const;
    /* The manifest an install would use: the verified remote release when it is newer, else the
       embedded (or dev) one. */
    const manifest::Manifest &ActiveManifest() const;
    void RunInstall();
    void RunUninstall();
    /* The progress page: shown before the engine starts and repainted after every file, because
       the engine blocks this thread while it works. */
    void BeginProgress(bool uninstall, int total);
    void UpdateProgress(const install::Progress &progress);

    void Render();
    void RenderHeader(Surface surface);
    /* The app's name and the one line under it, drawn no further right than `limit`: the header's
       tab strip computes that limit, so a longer language or a longer tab label cannot make the
       two overlap (measured in English: the subtitle reached under the "+" badge). */
    void DrawBrand(Surface surface, int limit);
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
    void RenderProgress(Surface surface);
    /* The chat-code explainer: + (or the header's "+" entry) opens it, pressing again or B
       goes back to where it was opened from. */
    void ToggleGuide();
    void RenderGuide(Surface surface);
    void RenderGameInstall(Surface surface);
    /* The two storage blocks in the header of the install page: microSD first, the console's own
       memory second.  They are also the control that picks where this run writes (design section
       8.1), so the text they show and the room they take both come from helpers the drawing and
       the touch targets share. */
    void StorageBarTexts(std::string text[2], bool ok[2], double used[2]) const;
    /* Everything the header's storage row needs, read and measured in one place: each storage's
       readout (its line of text, whether it could be read at all, how full it is) and the measured
       widths of that text and of the fixed "install to:" label.  The drawing and the touch targets
       both ask here -- only the font knows the widths, and a hit box that disagrees with the pixels
       is how a tap selects the storage the player was not pointing at. */
    void StorageRowReadout(std::string text[2], int text_w[2], bool ok[2], double used[2],
                           int *label_w);
    /* Reads and lays out the storage row once per frame into the members below: the ncm space
       queries are IPC calls, and both the header drawing and the touch targets need the same
       numbers -- asking twice per frame is twice the cost for one answer. */
    void RefreshStorageRow(int surface_width);
    /* Where the "install to:" label is drawn: one size up from the captions and bold. */
    int InstallToLabelSize() const;
    int InstallToLabelY() const;
    void RenderStorageBars(Surface surface);
    /* Draws the "L / R switch storage" note at `x` (the install page's own replacement for the
       "L / R switch pages" note, whose keys mean something else there); draws nothing when the
       note would run into the build stamp. */
    void DrawInstallTargetHint(Surface surface, int text_y, int x);
    /* Picks the storage a run writes to.  A package that is already open is re-opened on the new
       storage, because reading its CNMT/control data left placeholders behind on the old one. */
    void SetInstallTarget(install::TargetStorage target);
    /* True on the install screens where the target can still be changed (design section 8.1):
       picking a source, the file list, the address screen and the confirm card.  The transfer has
       the target locked in, and the result screen reports a run that is over -- a selectable-*looking*
       control there would be a control that does nothing. */
    bool InstallTargetSelectable() const;
    /* The install an interruption left behind: read from `install-task.json` (the engine writes it,
       see SetGameInstallTaskStore), and offered as "continue" or "give up" rather than silently
       holding space.  Design section 10 calls this the answer to "where did my gigabytes go?". */
    void RefreshPendingTask();
    /* The browser page's server (M4): it lives while the install page is up, and stops the moment
       the player leaves it -- "the server is only alive when it is needed" (design section 3). */
    void StartInstallerServer();
    void StopInstallerServer();
    /* Whether the page's server has anything to do right now: the two source screens print the
       address and its code, and a run those screens started keeps the page watching it through
       `/status.json`.  A file picked from the card needs no page at all. */
    bool InstallerServerNeeded() const;
    /* Brings the server in line with `InstallerServerNeeded()`: started when the player walks onto
       a source screen (or a run from one is still in hand), stopped when they walk off it.  Called
       once per frame after the input, so a screen that only just appeared is already served when it
       is drawn -- otherwise its first frame would say "the page service did not start". */
    void SyncInstallerServer();
    /* One non-blocking pass of the server, called from the UI loop and from the transfer loop
       between steps (so a browser sees progress and can cancel *while* bytes move). */
    void PollInstallerServer();
    /* One pass of the page's server, plus the hand-off it produced (a posted part goes to the upload
       source).  The frame loop, the transfer loop and the upload source's own wait all call this. */
    void ServiceInstallerPage();
    /* The route table, in one place: the page, its status, the address a browser pasted, and the
       cancel request. */
    net::HttpResponse ServeHttp(const net::HttpRequest &request);
    /* What `/status.json` answers: the console's own view of the install, so a browser (or a second
       tab, or another device) can show the same thing without the page keeping any state. */
    std::string InstallerStatusJson() const;
    /* The one line the result card puts under the title -- "written to microSD · 3 contents · 4.3
       GB", "this version is already installed", "a newer version (3.0.3) is installed", or the
       failure's reason.  Shared with the browser page's status payload, because two screens that
       describe the same run must not word it two ways. */
    std::string InstallOutcomeLine() const;
    /* "http://<ip>:<port>", the address the screens show and the QR code carries; empty when there
       is no network. */
    std::string InstallerAddressText() const;
    void RenderInstallPending(Surface surface);
    /* Picks the recorded run up again: the same source, opened the same way, and then adopted into
       the engine's task before any byte moves (see GameInstall::AdoptResume). */
    void ResumePendingInstall();
    /* Deletes what the interrupted run created -- its placeholders, and the task file itself --
       without touching anything another tool owns (no CleanupAllPlaceHolder). */
    void AbandonPendingInstall();
    /* Open the game installer, remembering where we came from (it is modal: B returns there). */
    void OpenGameInstall();
    bool TitleIsOurs(const std::string &title_id) const;
    /* "The agent can be installed once this is in" is only true when the version being installed
       (or already on the console) is the one this build's agent supports: the gate wants an exact
       version match, so 2.0.0 does not qualify just because the title is ours. */
    bool AgentSupportedFor(const std::string &title_id, std::uint32_t version) const;
    /* The version this build's agent handles for `title_id`, for the copy that has to name it
       ("... the agent only supports 3.0.3").  Empty when the title is not ours. */
    /* Every version this build's agent handles for `title_id`, joined for the copy.  It is a
       list, not one string, because the manifest may support several versions of the same game
       (3.0.3 today, 3.0.3 and 3.0.4 tomorrow) and the sentence has to keep telling the truth. */
    std::string AgentSupportedVersionsFor(const std::string &title_id) const;
    /* Confirmation pages share one implementation: action, outcome, two buttons. */
    void BuildConfirmActions(int width, int height, bool uninstall);
    void RenderConfirm(Surface surface, bool uninstall);
    void RenderResult(Surface surface);
    void Card(Surface surface, int x, int y, int w, int h, const char *title);

    /* Home-screen actions, rebuilt every frame from the same rectangles that get painted, so
       touch hit-testing and the painted buttons can never disagree. */
    void BuildHomeActions(int width, int height);
    void MoveHomeFocus(int direction);
    void NormalizeHomeFocus();
    void ActivateAction(int id);
    void HandleKeys(u32 down);
    /* The left stick drives the same focus the d-pad does, with a repeat that starts slow enough
       to land on one row and then accelerates. */
    void HandleStick(const PadState &pad);
    u64 m_stick_next_tick{0};
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
    /* Development aid: the first frame is the end of the black screen, so its launch offset is
       logged once (see install/dev_stats.hpp). */
    bool m_first_frame_traced{false};
    /* Development aid: the environment block of log.txt is written once, after the second frame --
       see TraceEnvironment. */
    bool m_report_traced{false};
    Page m_page{Page::Home};
    /* Where + sends the player back to when the guide page is closed again. */
    Page m_guide_return{Page::Home};
    /* Where the modal installer returns to when B is pressed (it is not a page stack: the
       installer can only be opened from a page, never from itself). */
    Page m_game_install_return{Page::Home};
    /* The installer has its own little screens: today "pick a source" and "pick a file"; the
       confirm / progress / result screens join them next. */
    enum class InstallScreen { Choose, Files, Url, Upload, Confirm, Progress, Result, Pending };
    InstallScreen m_install_screen{InstallScreen::Choose};
    /* A Home-screen helper started the embedded package directly, without the normal source
       picker.  Its result page returns to Home in one step. */
    bool m_install_returns_to_launcher{false};
    /* The address screen offers two ways in (the system keyboard now, the browser page that can
       submit an address once the server exists).  Neither works without a network, so the screen
       asks first and says so instead of letting a download fail a minute later. */
    bool m_install_no_network{false};
    /* Whether the page is the one driving the run in hand (it offered the file, or submitted the
       address).  A page left open from an earlier attempt can do that while the console is on any
       screen at all -- and then the run has to keep its server, or the page that started it loses
       the very status it is watching (2026-10-16). */
    bool m_install_page_drives{false};
    ui::ListView m_file_list;
    std::string m_file_dir{"/"};
    /* Reads the current directory into the list: directories first, then the files this app can
       install; everything else is listed dimmed so the player sees why it is not offered. */
    void RefreshFileList();
    void RenderInstallFiles(Surface surface);
    /* The download source's own screen: the address to type, and the reason it was refused when
       the address or the network did not hold up. */
    void RenderInstallUrl(Surface surface);
    /* The upload source's own screen: the same "open this address" block, with nothing above it
       (the page is the only way in, so the address and its code get the whole screen). */
    void RenderInstallUpload(Surface surface);
    /* The block the two source screens share: the caption, the address out of the running server,
       the line that says what the page is for, the waiting state, and the code.  Returns the y
       just past it.  `for_upload` selects that one line, which is the only thing that differs. */
    int RenderInstallerAddressBlock(Surface surface, int y, bool for_upload);
    /* The address the screens print, encoded once per address (the code is a pure function of it,
       and this runs every frame while the screen is up).  Empty when there is no address to print. */
    const util::QrCode &InstallerQrCode();
    /* Enters that screen: clears the last reason and asks the network question. */
    void EnterInstallUrl();
    /* Same question for the upload source, which reaches the network through the same server. */
    void EnterInstallUpload();
    /* Re-asks it (the screen's A while the "connect first" card is up, and the retry path). */
    void RefreshNetworkState();
    /* Lets go of the browser's source when a run is over (skipped, failed, installed): the page's
       next pick has to be accepted as a new run, not refused as a second one. */
    void ReleaseUploadSource(const char *why);
    /* The small print under the addresses: the rule an address has to satisfy, and what this app
       installs.  `with_range` is false in the failure state, where the strip above already says it. */
    void DrawInstallUrlNotes(Surface surface, int y, bool with_range);
    /* The failure the address screen is showing, in the player's language: the download source
       hands over short codes ("no-network", "range-unsupported", "http-status:404"), and anything
       else is its own sentence. */
    std::string InstallErrorText() const;
    /* Two footer hints side by side, each of them a control: what is drawn as pressable has to be
       pressable.  The confirm screen and the address screen both use it. */
    /* Draws the two hints and returns the x just past the second one, so a caller can put its own
       note after the pair without repeating the spacing arithmetic. */
    int DrawFooterHintPair(Surface surface, int y, i18n::StringId first, int first_action,
                           i18n::StringId second, int second_action);
    void LeaveDirectory();
    void PickFocusedFile();
    /* Opens the picked package and decides what to show next: the confirm screen, or the result
       screen when the package turns out to be pointless (already installed / newer installed). */
    void OpenPickedPackage();
    /* The part of opening a package that does not care where its bytes come from: the CNMT/NACP
       read, the confirm screen, or the result screen when the package turns out to be pointless.
       The local file and the download address both land here. */
    /* `source` and `url` are recorded in install-task.json: a resume has to know *where* the bytes
       came from, and only the app knows that (the engine just reads whatever reader it was given). */
    void OpenPackage(install::ContainerReader &reader, const std::string &name, std::uint64_t size,
                     install::TaskSource source, const std::string &url);
    /* Asks for an address (system keyboard), probes it and opens the package it names. */
    void StartDownload();
    /* Opens the package an address names (the same path for a freshly typed address and for the
       re-open after the target storage changed).  False leaves the address screen showing why. */
    bool OpenDownloadPackage(const std::string &address);
    void RunGameInstall();
    void RefreshForwarderTile();
    void InstallForwarderTile();
    void RemoveForwarderTile();
    void DisableAutoSleep();
    /* The agent's own install, on this thread: the payload that matches the manifest's origin,
       the engine, and the per-file progress.  It owns no screen and decides no result -- the home
       screen's button and the step that follows a game install call it and report their own way. */
    bool InstallAgentNow(int *files_written, std::string *error,
                         const std::function<void(const install::Progress &)> &on_progress);
    /* What the step after a game install did.  `AlreadyCurrent` is the "nothing to write" case;
       `Failed` carries the reason in m_agent_follow_reason (log + details), while the screen shows
       the one sentence the player can act on. */
    enum class AgentFollowUp { NotRun, AlreadyCurrent, Installed, Failed };
    AgentFollowUp m_agent_follow_up{AgentFollowUp::NotRun};
    std::string m_agent_follow_reason;
    /* The installer's progress card is showing that follow-up step rather than the package's own
       transfer: the same card, with the numbers that step actually has. */
    bool m_install_follow_up{false};
    /* Installs the agent when the run in hand leaves the console on a version the agent supports
       (a base game or an update; an add-on or a data patch changes no version, and a version the
       manifest does not name cannot be worked with at all).  Does nothing otherwise. */
    void RunAgentFollowUp();
    /* Leaves the installer page: the package in hand belongs to the screen that was showing it,
       so it is dropped here and nowhere else (B is the only way out of the page). */
    void LeaveGameInstall();
    /* True while the agent's files are in place (and the state file says so): the one answer to
       "is there an agent to install?", used by the home screen's buttons and by the installer's
       result page. */
    bool AgentInstalled() const;
    /* One step back inside the installer: B and the footer's Ⓑ hint are two spellings of one
       control, so both come through here and cannot drift apart.  From the source screen this
       leaves the page. */
    void InstallStepBack();
    /* L / R: one step through the pages the player reads (status / details / guide), wrapping at
       both ends.  See the definition for what happens on a page outside that ring. */
    void SwitchPeerPage(int direction);
    /* Drops the package in hand without leaving the installer: the parsed container, the
       placeholders the CNMT/NACP read created, and the decoded cover.  What a package that was
       never installed owes the console, and nothing more -- a run that reached the register step
       has already given the console its content and its record. */
    void LeaveInstallScreen();
    void RenderInstallConfirm(Surface surface);
    void RenderInstallProgress(Surface surface);
    void RenderInstallResult(Surface surface);
    void RenderStartupCheck(Surface surface);
    void RepairStartup();
    /* "Animal Crossing: New Horizons 3.0.3 (Update)" -- the run in hand named in one line; the
       confirm, progress and result cards all show it, so what is being installed never has to be
       guessed at. */
    std::string PackageSummary() const;
    /* The cover of the game the package belongs to, decoded once when the package is opened and
       drawn on the card's right-hand side.  Returns the width the card's text may still use. */
    int DrawPackageCover(Surface surface, int card_x, int card_top, int card_w, int card_h);
    std::string m_picked_file;
    /* Which screen started the run in hand: "back" on the confirm and result screens returns here
       (the file list for a card install, the input screen for a URL or an upload). */
    InstallScreen m_install_origin{InstallScreen::Choose};
    std::unique_ptr<install::LocalFileSource> m_file_source;
    /* Where this run would write: the header's two storage blocks select it, and the choice is
       remembered in settings.json (design section 8.1).  Read at startup, so the page opens on the
       storage the player picked last time. */
    install::TargetStorage m_install_target{install::TargetStorage::SdCard};
    /* The installer's own web server.  `m_http_port` is the port it really got (the fixed default,
       or a kernel-assigned one when that is taken) -- the address on screen is built from it. */
    net::HttpServer m_http_server;
    std::uint16_t m_http_port{0};
    /* The address the source screens are printing, encoded: `m_address_qr_text` is what the cache
       was built from, so a port that changed (or a server that stopped) re-encodes exactly once. */
    util::QrCode m_address_qr;
    std::string m_address_qr_text;
    /* An address the *browser* handed over (`POST /url`): the UI loop picks it up on the next frame
       and opens it exactly as if the player had typed it, because that is what it is. */
    std::string m_http_pending_url;
    /* The run in hand as a number the page can quote.  It changes every time a package is opened --
       including the re-open that a storage switch does -- so a page that is out of date cannot start
       or retarget a run it was never told about (server-side gate; the page's own peer check is only
       a courtesy). */
    int m_run_id{0};
    /* Two requests from the page that change what the console is doing.  Both are taken by the UI
       loop rather than by the socket pass, because both touch ncm/engine state. */
    bool m_http_pending_start{false};
    bool m_http_pending_back{false};
    bool m_http_pending_target{false};
    install::TargetStorage m_http_target{install::TargetStorage::SdCard};
    /* The two storage blocks as the status payload reports them: refreshed once a second, not on
       every request (the numbers come from ncm, and the page only needs them to look current). */
    std::string m_status_storage_text[2];
    bool m_status_storage_ok[2]{false, false};
    double m_status_storage_used[2]{0.0, 0.0};
    std::uint64_t m_status_storage_ms{0};
    void RefreshStatusStorage();
    /* The one line under the confirm card's rows: the agent can follow this package, cannot, or this
       package is none of its business.  Shared with the page, so both devices say the same sentence. */
    std::string PackageAgentNote(bool *warn) const;
    /* The progress card's two notes, built once here so the console's card and the page's status
       payload print the same words (and the same rounding) for the same numbers.  Empty when the
       installer has no honest answer: nothing moved in the window, or nothing is left. */
    std::string InstallRateText() const;
    std::string InstallEtaText() const;
    /* This frame's storage row (see RefreshStorageRow).  `m_storage_row_ready` is cleared at the
       end of every frame so nothing can be drawn from a stale read. */
    bool m_storage_row_ready{false};
    std::string m_storage_text[2];
    int m_storage_text_w[2]{0, 0};
    bool m_storage_ok[2]{false, false};
    double m_storage_used[2]{0.0, 0.0};
    ui::StorageBarsLayout m_storage_row{};
    /* The startup self-check, when main found the running NRO outside the standard path. */
    env::StartupPlan m_startup;
    bool m_startup_page{false};
    bool m_startup_repaired{false};
    bool m_startup_restart{false};
    std::string m_startup_error;
    /* The download source, kept alive for as long as the run in hand needs to read from it (the
       engine holds a reference, not a copy). */
    std::unique_ptr<install::HttpSource> m_http_source;
    /* The upload run's source: the browser is the file, and this is how the engine reads it. */
    std::unique_ptr<install::HttpUploadSource> m_upload_source;
    std::unique_ptr<install::MemorySource> m_memory_source;
    /* True while this thread is inside Prepare() for an upload: the console is reading the package
       over the page, and the page's own wait pump redraws the countdown.  Only ever set on the UI
       thread -- the transfer's reads happen on the reader thread and must not draw. */
    bool m_upload_reading{false};
    /* What `POST /upload/begin` asked for: the UI loop opens it like any other package (the file is
       remote, everything after Prepare is the same code). */
    bool m_http_pending_upload{false};
    std::string m_http_upload_name;
    std::uint64_t m_http_upload_size{0};
    /* The id the page quotes with every part; a stale tab's parts are refused (409).  The serial
       makes two runs in one session distinguishable. */
    std::string m_http_upload_task_id;
    unsigned m_http_upload_serial{0};
    /* The address the run in hand was started from, shown back on the address screen so a typo is
       visible without opening the keyboard again. */
    std::string m_download_url;
    /* The run in hand when it is a *resume*: the recorded task, adopted into the engine right after
       the package has been read (its ids and byte counts are what pick up where the interruption
       stopped).  Cleared with the package. */
    const install::InstallTask *m_resume_task{nullptr};
    /* Bytes the interrupted run had already put in when this one picked it up: 0 for a fresh run.
       The confirm card says so (the player has to know they are continuing, not starting over). */
    std::uint64_t m_resumed_bytes{0};
    /* What `install-task.json` says, refreshed when the install page opens and when a run ends. */
    bool m_have_pending_task{false};
    install::InstallTask m_pending_task;
    /* What the console still *holds*, asked of ncm rather than read from the file: a reboot clears
       content-storage placeholders (measured 2026-10-16), so the file's byte counts can describe
       bytes nobody has.  These are the numbers the screens show. */
    std::uint64_t m_pending_live_bytes{0};
    std::uint64_t m_pending_live_reserved{0};
    /* The file says an install had started, but nothing of it is left: continuing will start over. */
    bool m_pending_restarts{false};
    std::unique_ptr<install::GameInstall> m_game_install;
    bool m_forwarder_installed{false};
    bool m_forwarder_application_running{false};
    install::InstallStatus m_install_status;
    std::string m_install_error;
    ui::Bitmap m_cover{};
    /* The mini-app's code, read once from the embedded asset (empty when the build has none). */
    ui::QrMatrix m_guide_qr{};
    i18n::Language m_language{i18n::Language::ZhHans};
    Touch m_touch{};
    /* Touch press tracking: a tap only fires when the finger goes down and comes back up on
       the same action (Sphaira-style), so a drag never triggers a button by accident. */
    TapTracker m_tap{};
    std::vector<Action> m_actions{};
    int m_focus{-1};
    bool m_home_focus_needs_default{true};
    bool m_should_exit{false};

    HomeKind m_home_kind{HomeKind::NeedsInstall};
    bool m_last_failed{false};
    /* The record says installed, but the card no longer matches it. */
    bool m_files_incomplete{false};

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

    /* What the running install/uninstall is working on (the engine reports it per file). */
    bool m_progress_uninstall{false};
    std::string m_progress_step{};
    int m_progress_index{0};
    int m_progress_total{0};
    bool m_result_ok{false};
    std::string m_result_error{};
    int m_result_files{0};
    /* Whether the last action was an uninstall (the result page picks its wording). */
    bool m_result_uninstall{false};
    bool m_result_forwarder{false};
    net::UpdateCheckTask m_update_task{};
    bool m_update_running{false};
    /* A startup check keeps a failure to itself (the button keeps its normal wording); a
       check the player asked for always reports what happened. */
    bool m_update_silent{false};
    bool m_update_have_result{false};
    net::UpdateOutcome m_update_outcome{net::UpdateOutcome::Network};
    long m_update_http_code{0};
    std::string m_update_detail{};
    std::string m_update_remote_version{};
    /* The release host has something newer, but it does not cover this console's build: the
       bundled release stays the install source and the result says so. */
    bool m_update_newer_unsupported{false};
    /* A verified remote manifest that is newer than what is installed; the install path uses it
       instead of the embedded release while it stands. */
    manifest::Manifest m_update_manifest{};
    bool m_have_update_manifest{false};
    /* Log every drawing stage of the first frame, so a crash points at one of them. */
    int m_trace_frames{1};
    /* First-frame stage pauses (when dev-pause exists): wait for + after each stage. */
    bool m_stage_pause{false};
    void WaitForPlus(const char *stage);
};

}  // namespace acnh_manager::ui
