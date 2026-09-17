#include <switch.h>

#include <fcntl.h>
#include <cstdio>
#include <unistd.h>

#include "env/detect.hpp"
#include "install/gate.hpp"
#include "log.hpp"
#include "probe.hpp"
#include "ui/app.hpp"
#include "ui/text_ui.hpp"

#ifndef ACNH_BUILD_STAMP
#define ACNH_BUILD_STAMP "unknown"
#endif

namespace {

constexpr const char *kBuildStamp = ACNH_BUILD_STAMP;
constexpr const char *kAppDir = "/switch/ACNH-Manager";
constexpr const char *kLogPath = "/switch/ACNH-Manager/log.txt";
constexpr const char *kLogHistoryPath = "/switch/ACNH-Manager/log-history.log";
constexpr const char *kProbeFlagPath = "/switch/ACNH-Manager/dev-probe";
constexpr const char *kTextUiFlagPath = "/switch/ACNH-Manager/ui-text";
constexpr const char *kStdioPath = "/switch/ACNH-Manager/stdout.log";
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

/* 与 EdiZon-SE 的做法对齐:进界面之前把常用服务与时钟准备好,并把 stdout/stderr
   重定向到 SD 上的文件(避免任何库输出落到未初始化的控制台上)。
   失败不致命,逐项记录到日志。 */
void PrepareEnvironment(acnh_manager::Log *log) {
    fsdevMountSdmc();
    /* 注意:**不要**动 STDOUT/STDERR 的 fd:我们与 hbl 加载器同进程,dup2 会改到它的
       stdio 状态(实测会导致加载器在 stdio 缓冲路径里崩溃)。诊断一律走 Log 的文件写入。 */
    (void)kStdioPath;

    struct Step {
        const char *name;
        Result (*init)();
    };
    const Step steps[] = {
        {"setsysInitialize", setsysInitialize},
        {"socketInitializeDefault", socketInitializeDefault},
        {"plInitialize", []() { return plInitialize(PlServiceType_User); }},
        {"psmInitialize", psmInitialize},
        {"pminfoInitialize", pminfoInitialize},
        {"pmdmntInitialize", pmdmntInitialize},
        {"romfsInit", romfsInit},
        {"hidsysInitialize", hidsysInitialize},
        {"pcvInitialize", pcvInitialize},
        {"clkrstInitialize", clkrstInitialize},
    };
    for (const Step &step : steps) {
        const Result rc = step.init();
        if (log != nullptr) {
            log->Line("env: %s rc=0x%08X", step.name, rc);
        }
    }
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

    /* 刻意不使用 libnx 控制台:在 hbl 环境里它会占用默认窗口,与界面 framebuffer 抢同一份状态
       (实测会引起加载器进程崩溃)。所有诊断写 log.txt,界面自己负责显示。 */
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
        log.Line("build: %s (ACNH-Manager %s)", kBuildStamp, kAppVersion);
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
    }

    /* 环境准备(对齐 EdiZon-SE 的 serviceInitialize + stdio 重定向)。 */
    if (R_SUCCEEDED(rc_log)) {
        log.Line("env: preparing services");
        log.Sync();
        /* 图形路径不再往控制台/stdio 回显:避免触碰同进程内 hbl 的 stdio。 */
        log.SetEcho(false);
    }
    PrepareEnvironment(R_SUCCEEDED(rc_log) ? &log : nullptr);

    /* 界面模式:默认图形界面;`/switch/ACNH-Manager/ui-text` 存在时退回控制台文本界面。 */
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

    /* 图形界面路径(实验性):日志保持打开,各阶段都记录,便于崩溃后取证。 */
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
            app.Run();
            app.Exit();
            if (R_SUCCEEDED(rc_log)) {
                log.Line("ui: loop exited");
            }
        } else {
            if (R_SUCCEEDED(rc_log)) {
                log.Line("ui init failed: %s", ui_error.c_str());
            }
            /* 没有控制台可用,只能等用户按 + 退出;失败原因已写进 log.txt。 */
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
