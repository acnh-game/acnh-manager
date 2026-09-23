#include <switch.h>

#include <fcntl.h>
#include <cstdio>
#include <unistd.h>

#include "env/detect.hpp"
#include "install/gate.hpp"
#include "log.hpp"
#include "manifest/manifest.hpp"
#include "payload/embedded.hpp"
#include "probe.hpp"
#include "ui/app.hpp"
#include "ui/text_ui.hpp"
#include "util/fs_path.hpp"
#include "version.hpp"

#ifndef ACNH_BUILD_STAMP
#define ACNH_BUILD_STAMP "unknown"
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
constexpr const char *kProbeFlagPath = "/switch/ACNH-Manager/dev-probe";
constexpr const char *kWriteProbeFlagPath = "/switch/ACNH-Manager/dev-writeprobe";
constexpr const char *kFsProbeFlagPath = "/switch/ACNH-Manager/dev-fsprobe";
constexpr const char *kTouchProbeFlagPath = "/switch/ACNH-Manager/dev-touchprobe";
constexpr const char *kTextUiFlagPath = "/switch/ACNH-Manager/ui-text";
constexpr const char *kStdioPath = "/switch/ACNH-Manager/stdout.log";

const char *AppletTypeName(int type) {
    switch (type) {
        case AppletType_Application: return "Application";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_LibraryApplet: return "LibraryApplet";
        case AppletType_SystemApplet: return "SystemApplet";
        case AppletType_OverlayApplet: return "OverlayApplet";
        default: return "?";
    }
}

const char *MetaTypeName(u8 type) {
    switch (type) {
        case 0x80: return "Application";
        case 0x81: return "Patch";
        case 0x82: return "AddOnContent";
        case 0x83: return "Delta";
        default: return "?";
    }
}

const char *GateStatusName(acnh_manager::install::GateStatus status) {
    using acnh_manager::install::GateStatus;
    switch (status) {
        case GateStatus::Supported: return "supported";
        case GateStatus::NoManifest: return "no-manifest";
        case GateStatus::TitleNotSupported: return "title-not-supported";
        case GateStatus::VersionNotSupported: return "version-not-supported";
        case GateStatus::ContentIdMissing: return "content-id-missing";
        case GateStatus::ContentIdMismatch: return "content-id-mismatch";
        case GateStatus::BuildIdMismatch: return "build-id-mismatch";
    }
    return "?";
}

bool FileExists(FsFileSystem &sd, const char *path) {
    FsFile file{};
    const acnh_manager::util::FsPath arg(path);
    if (R_FAILED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file))) {
        return false;
    }
    fsFileClose(&file);
    return true;
}

/* Environment setup: we only use the raw fs* / ns / ncm / pl / time / pad interfaces and
   deliberately avoid mounting devoptab (fsdevMountSdmc), redirecting stdio, or bringing up
   extra services.  Inside hbl we share the process with the loader, and touching stdio,
   devoptab or extra services can bite us (the crash we chased landed in the loader's
   devoptab buffer path).  Diagnostics always go to the Log file instead. */
void PrepareEnvironment(acnh_manager::Log *log) {
    (void)log;
}

void ReportEnvironment(acnh_manager::Log &log,
                       const acnh_manager::env::EnvironmentReport &report) {
    using acnh_manager::install::Evaluate;
    log.Line("== ACNH-Manager %s ==", acnh_manager::kAppVersion);
    log.Line("HOS %s, applet=%s", report.hos_version.c_str(), AppletTypeName(report.applet_type));
    log.Line("game running=%d, patch storage=%u", report.application_running ? 1 : 0,
             report.patch_found ? report.patch_storage : 0xFF);
    for (const auto &meta : report.metas) {
        log.Line("  meta type=%s(%#04x) storage=%u version=%u(%#x)", MetaTypeName(meta.meta_type),
                 meta.meta_type, meta.storage, meta.version, meta.version);
    }
    log.Line("detected build: title=%s version=%u contentId=%s", report.build.title_id.c_str(),
             report.build.version,
             report.build.content_id.empty() ? "(unavailable)" : report.build.content_id.c_str());
    if (report.build.module_id_known) {
        log.Line("  running main ModuleId=%s (dmnt:cht)", report.build.module_id.c_str());
    }
    log.Line("override advice: %s", report.advice.text.c_str());
    log.Line("exefs override files: %zu", report.exefs.size());
    for (const auto &file : report.exefs) {
        log.Line("  %s (%llu B)", file.name.c_str(), static_cast<unsigned long long>(file.size));
    }
    if (report.legacy_cheat_present) {
        log.Line("legacy cheat present: %s entry=\"%s\" (same chat path as the agent)",
                 report.legacy_cheat_path.c_str(),
                 report.legacy_cheat_entry.empty() ? "(file unreadable)"
                                                   : report.legacy_cheat_entry.c_str());
    }
    if (!report.problems.empty()) {
        log.Line("problems: %s", report.problems.c_str());
    }

    /* The manifest this build installs from: the embedded release channel, which the interface
       calls "bundled with the app".  This block used to evaluate with a null manifest and print a
       hard-coded "no embedded release manifest", written before the release import existed -- so
       every real build logged a gate of `no-manifest` while its own screen said `supported`,
       and log.txt is what support reads.  The network check runs later and may adopt a newer
       manifest; what it adopted is reported by the details page and by the update-check rows, not
       here (this line is about what the build carries). */
    acnh_manager::manifest::Manifest active;
    std::string manifest_line;
    bool have_manifest = false;
    if (acnh_manager::payload::EmbeddedAvailable()) {
        const auto parsed = acnh_manager::manifest::Parse(
            acnh_manager::payload::EmbeddedManifestJson(), acnh_manager::kAppVersion);
        if (parsed.ok) {
            active = parsed.manifest;
            have_manifest = true;
            manifest_line = "embedded, agent " + active.agent.version + " (commit " +
                            active.agent.commit + ")";
        } else {
            manifest_line = "embedded manifest rejected: " + parsed.error;
        }
    } else {
        manifest_line = "none embedded (this build carries no release import)";
    }
    const auto gate = Evaluate(have_manifest ? &active : nullptr, report.build);
    log.Line("gate: %s (%s)", GateStatusName(gate.status), gate.reason.c_str());
    log.Line("manifest: %s", manifest_line.c_str());
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

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
        rc_log = log.Open(sd, kLogPath, true);
        if (R_SUCCEEDED(rc_log)) {
            log.Open(sd, kLogHistoryPath, false);
        }
    }

    if (R_SUCCEEDED(rc_log)) {
        log.Line("build: %s (ACNH-Manager %s)", kBuildStamp, acnh_manager::kAppVersion);
        log.Line("start: fsInitialize rc=0x%08X openSdmc rc=0x%08X mkdir rc=0x%08X", rc_fs, rc_sd,
                 rc_dir);
        const auto report = acnh_manager::env::Collect(sd);
        ReportEnvironment(log, report);
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

    /* UI mode: graphics by default; `/switch/ACNH-Manager/ui-text` falls back to the
       console text UI. */
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
                log.Sync();
            }
            /* Touch probe runs with the first frame already on screen, so the log tells us
               both "did hid give us touch" and "what does a tap look like", with the user
               able to see the UI while they tap. */
            if (R_SUCCEEDED(rc_log) && FileExists(sd, kTouchProbeFlagPath)) {
                log.Line("dev-touchprobe flag present: running the touch probe");
                acnh_manager::probe::RunTouchProbe(log, sd);
            }
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
