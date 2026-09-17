#include <switch.h>

#include <cstdio>

#include "env/detect.hpp"
#include "install/gate.hpp"
#include "log.hpp"
#include "probe.hpp"
#include "ui/app.hpp"

namespace {

constexpr const char *kAppDir = "/switch/ACNH-Manager";
constexpr const char *kLogPath = "/switch/ACNH-Manager/log.txt";
constexpr const char *kLogHistoryPath = "/switch/ACNH-Manager/log-history.log";
constexpr const char *kProbeFlagPath = "/switch/ACNH-Manager/dev-probe";
/* 与 Makefile 的 APP_VERSION 保持一致。 */
constexpr const char *kAppVersion = "0.1.0";

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
    if (R_FAILED(fsFsOpenFile(&sd, path, FsOpenMode_Read, &file))) {
        return false;
    }
    fsFileClose(&file);
    return true;
}

void ReportEnvironment(acnh_manager::Log &log,
                       const acnh_manager::env::EnvironmentReport &report) {
    using acnh_manager::install::Evaluate;
    log.Line("== ACNH-Manager %s ==", kAppVersion);
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
        log.Line("legacy cheat present: %s(与 agent 可能冲突,建议移入备份)",
                 report.legacy_cheat_name.c_str());
    }
    if (!report.problems.empty()) {
        log.Line("problems: %s", report.problems.c_str());
    }

    /* M1 阶段没有内置清单:门控会落回 NoManifest,这里如实显示。 */
    const auto gate = Evaluate(nullptr, report.build);
    log.Line("gate: %s (%s)", GateStatusName(gate.status), gate.reason.c_str());
    log.Line("manifest: 未内置发布清单,等待 M4 的发布导入工具生成");
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);
    std::printf("ACNH-Manager %s\nlog: %s\n", kAppVersion, kLogPath);
    consoleUpdate(nullptr);

    const Result rc_fs = fsInitialize();
    FsFileSystem sd{};
    const Result rc_sd = R_SUCCEEDED(rc_fs) ? fsOpenSdCardFileSystem(&sd) : rc_fs;
    Result rc_dir = rc_sd;
    acnh_manager::Log log;
    Result rc_log = rc_sd;
    if (R_SUCCEEDED(rc_sd)) {
        rc_dir = fsFsCreateDirectory(&sd, kAppDir);
        rc_log = log.Open(sd, kLogPath, true);
        if (R_SUCCEEDED(rc_log)) {
            log.Open(sd, kLogHistoryPath, false);
        }
    }

    if (R_SUCCEEDED(rc_log)) {
        log.Line("start: fsInitialize rc=0x%08X openSdmc rc=0x%08X mkdir rc=0x%08X", rc_fs, rc_sd,
                 rc_dir);
        const auto report = acnh_manager::env::Collect(sd);
        ReportEnvironment(log, report);
        if (FileExists(sd, kProbeFlagPath)) {
            log.Line("dev-probe flag present: running the M0 environment probe as well");
            acnh_manager::probe::Run(log, sd);
        }
        log.Line("=== done: press + to exit ===");
        log.Sync();
        log.Close();
    } else {
        std::printf("fatal: cannot open %s (sdmc rc=0x%08X log rc=0x%08X)\n", kLogPath, rc_sd,
                    rc_log);
        consoleUpdate(nullptr);
    }

    /* 界面模式:环境报告已经落在 log.txt,这里进入 framebuffer 界面。 */
    {
        acnh_manager::ui::App app;
        if (app.Init(sd)) {
            app.Run();
            app.Exit();
        } else {
            std::printf("ui init failed (fonts or framebuffer); press + to exit\n");
            consoleUpdate(nullptr);
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            while (appletMainLoop()) {
                padUpdate(&pad);
                if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
                    break;
                }
                consoleUpdate(nullptr);
            }
        }
    }
    consoleExit(nullptr);
    return 0;
}
