#include <switch.h>

#include <fcntl.h>
#include <cstdio>
#include <unistd.h>

#include "install/dev_stats.hpp"
#include "log.hpp"
#include "ui/app.hpp"
#include "util/fs_path.hpp"
#include "version.hpp"

/* The probes and the text-UI fallback are development aids: the makefile leaves them out of a
   release build entirely (see DEV_AIDS there), which is also why their includes are behind the
   same switch -- a released binary should not even carry the strings. */
#if ACNH_DEV_AIDS
#include "probe.hpp"
#include "ui/text_ui.hpp"
#endif

#ifndef ACNH_BUILD_STAMP
#define ACNH_BUILD_STAMP "unknown"
#endif

#ifndef ACNH_DEV_AIDS
#define ACNH_DEV_AIDS 0
#endif

/* Exiting: hand the applet back to the applet manager instead of returning to the loader.

   hbl (the loader the album uses) keeps one process for the whole album session and reloads
   every NRO inside it, so whatever an NRO does not give back to the system stays behind.  The
   default window init asks the applet manager for a managed display layer on every load and
   only closes the layer on exit, while the manager destroys applet layers when the *applet*
   terminates, not when an NRO returns.  Launching and exiting homebrew inside one album
   session therefore piles up layers; on hardware the third exit already left the album on a
   black screen even though the app logged a clean exit (see docs/architecture.md).

   Setting this to 1 makes the exit path run the applet exit commands even when we were not
   launched as a real title (the same thing DBI does), so the applet terminates and the applet
   manager frees everything it owns.  The trade-off is that leaving the app lands on the home
   menu instead of back in the loader's menu. */
extern "C" {
u32 __nx_applet_exit_mode = 1;
}

namespace {

constexpr const char *kBuildStamp = ACNH_BUILD_STAMP;
constexpr const char *kAppDir = "/switch/ACNH-Manager";
constexpr const char *kLogPath = "/switch/ACNH-Manager/log.txt";
constexpr const char *kLogHistoryPath = "/switch/ACNH-Manager/log-history.log";
#if ACNH_DEV_AIDS
constexpr const char *kProbeFlagPath = "/switch/ACNH-Manager/dev-probe";
constexpr const char *kWriteProbeFlagPath = "/switch/ACNH-Manager/dev-writeprobe";
constexpr const char *kFsProbeFlagPath = "/switch/ACNH-Manager/dev-fsprobe";
/* Development switch for the "how does this card handle a file larger than 4 GiB?" probe; it
   writes one 4.5 GB file in the manager's own directory and deletes it again. */
constexpr const char *kLargeFileProbeFlagPath = "/switch/ACNH-Manager/dev-largefileprobe";
constexpr const char *kTouchProbeFlagPath = "/switch/ACNH-Manager/dev-touchprobe";
/* Development switch for the network probe: what limits the download path (see probe.hpp). */
constexpr const char *kNetProbeFlagPath = "/switch/ACNH-Manager/dev-netprobe";
constexpr const char *kTextUiFlagPath = "/switch/ACNH-Manager/ui-text";
#endif
/* Dev flags are the only way this is used, so the release build does not carry it either (an
   unused static function is a warning, and this build is kept at zero). */
#if ACNH_DEV_AIDS
bool FileExists(FsFileSystem &sd, const char *path) {
    FsFile file{};
    const acnh_manager::util::FsPath arg(path);
    if (R_FAILED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file))) {
        return false;
    }
    fsFileClose(&file);
    return true;
}
#endif

/* Environment setup: we only use the raw fs* / ns / ncm / pl / time / pad interfaces and
   deliberately avoid mounting devoptab (fsdevMountSdmc), redirecting stdio, or bringing up
   extra services.  Inside hbl we share the process with the loader, and touching stdio,
   devoptab or extra services can bite us (the crash we chased landed in the loader's
   devoptab buffer path).  Diagnostics always go to the Log file instead. */
void PrepareEnvironment(acnh_manager::Log *log) {
    (void)log;
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* Development aid: the start of the clock the "launch: +N ms" lines are measured against (a
       no-op in a release build). */
    acnh_manager::install::dev::MarkLaunch();

    /* libnx's console is deliberately not used: inside hbl it takes the default window and
       fights the UI framebuffer for the same state (crashed the loader process on real
       hardware).  Diagnostics go to log.txt; the UI paints its own framebuffer. */
    const Result rc_fs = fsInitialize();
    FsFileSystem sd{};
    const Result rc_sd = R_SUCCEEDED(rc_fs) ? fsOpenSdCardFileSystem(&sd) : rc_fs;
    Result rc_dir = rc_sd;
    acnh_manager::Log log;
    Result rc_log = rc_sd;
    if (R_SUCCEEDED(rc_sd)) {
        rc_dir = fsFsCreateDirectory(&sd, acnh_manager::util::FsPath(kAppDir).c_str());
        /* The log switch is a runtime one (see source/log.hpp): read it here, once, before any line
           is written.  A development build always writes; a release build only when `log-on` is on
           the card, and then it writes a log the same as any other build. */
        const bool want_log = acnh_manager::LogWanted(sd);
        rc_log = log.Open(sd, kLogPath, true, want_log);
        if (R_SUCCEEDED(rc_log)) {
            log.Open(sd, kLogHistoryPath, false, want_log);
        }
    }

    if (R_SUCCEEDED(rc_log)) {
        log.Line("build: %s (ACNH-Manager %s)", kBuildStamp, acnh_manager::kAppVersion);
        log.Line("start: fsInitialize rc=0x%08X openSdmc rc=0x%08X mkdir rc=0x%08X", rc_fs, rc_sd,
                 rc_dir);
        acnh_manager::install::dev::LogStartup(log, "fs + SD + log ready");
        /* The environment block (HOS, metas, build, gate, manifest) is written by the UI, off the
           one collection it does -- see App::TraceEnvironment.  It used to be collected here as
           well, and doing the same work twice cost the launch about 1.6 s on hardware. */
#if ACNH_DEV_AIDS
        if (FileExists(sd, kProbeFlagPath)) {
            log.Line("dev-probe flag present: running the M0 environment probe as well");
            acnh_manager::probe::Run(log, sd);
        }
        if (FileExists(sd, kWriteProbeFlagPath)) {
            log.Line("dev-writeprobe flag present: running the SD write probe");
            acnh_manager::probe::RunWriteProbe(log, sd);
        }
        if (FileExists(sd, kFsProbeFlagPath)) {
            log.Line("dev-fsprobe flag present: probing the fs session at startup");
            acnh_manager::probe::RunFsSessionProbe(log, sd, "startup");
        }
        if (FileExists(sd, kLargeFileProbeFlagPath)) {
            log.Line("dev-largefileprobe flag present: measuring how this card handles >4 GiB");
            acnh_manager::probe::RunLargeFileProbe(log, sd);
        }
        if (FileExists(sd, kNetProbeFlagPath)) {
            log.Line("dev-netprobe flag present: measuring what limits the download path");
            acnh_manager::probe::RunNetworkProbe(log, sd);
        }
#endif
        log.Line("=== done: press + to exit ===");
        log.Sync();
    }

    /* Environment setup (aligned with EdiZon-SE's serviceInitialize + stdio redirect). */
    if (R_SUCCEEDED(rc_log)) {
        log.Line("env: minimal (no devoptab, no stdio redirect, no extra services)");
        log.Sync();
        /* The graphics path no longer echoes to the console or stdio: avoid touching the
           loader's stdio inside the same process. */
    }
    PrepareEnvironment(R_SUCCEEDED(rc_log) ? &log : nullptr);

    /* UI mode: graphics by default; `/switch/ACNH-Manager/ui-text` falls back to the console text
       UI (a development aid, so a release build only has the graphics path). */
#if ACNH_DEV_AIDS
    const bool want_text_ui = FileExists(sd, kTextUiFlagPath);
    if (want_text_ui) {
        if (R_SUCCEEDED(rc_log)) {
            log.Line("text-ui: starting");
            log.Sync();
        }
        acnh_manager::ui::TextUi ui;
        if (ui.Init()) {
            ui.Run(R_SUCCEEDED(rc_log) ? &log : nullptr, sd);
            ui.Exit();
        }
        if (R_SUCCEEDED(rc_log)) {
            log.Line("text-ui: finished");
            log.Sync();
            log.Close();
        }
        return 0;
    }
#endif

    /* Graphics path: the log stays open and every stage is recorded, so a crash leaves
       evidence behind. */
    {
        acnh_manager::ui::App app;
        std::string ui_error;
        if (R_SUCCEEDED(rc_log)) {
            log.Line("ui: initialising");
            log.Sync();
        }
        if (app.Init(R_SUCCEEDED(rc_log) ? &log : nullptr, sd, &ui_error)) {
            if (R_SUCCEEDED(rc_log)) {
                log.Line("ui: init ok (fonts + framebuffer ready)");
                /* Init draws one frame of its own before returning (that is where the black screen
                   ends), so this line is the tail of the startup, not the start of the loop. */
                acnh_manager::install::dev::LogStartup(log, "init returned, entering the loop");
                log.Sync();
            }
            /* Touch probe runs with the first frame already on screen, so the log tells us
               both "did hid give us touch" and "what does a tap look like", with the user
               able to see the UI while they tap. */
#if ACNH_DEV_AIDS
            if (R_SUCCEEDED(rc_log) && FileExists(sd, kTouchProbeFlagPath)) {
                log.Line("dev-touchprobe flag present: running the touch probe");
                acnh_manager::probe::RunTouchProbe(log, sd);
            }
#endif
            app.Run();
            app.Exit();
            if (R_SUCCEEDED(rc_log)) {
                log.Line("ui: loop exited");
            }
        } else {
            if (R_SUCCEEDED(rc_log)) {
                log.Line("ui init failed: %s", ui_error.c_str());
            }
            /* No console available: wait for + and exit; the reason is already in log.txt. */
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
                    break;
                }
            }
        }
        if (R_SUCCEEDED(rc_log)) {
            log.Line("app: exiting");
            log.Sync();
            log.Close();
        }
    }
    return 0;
}
