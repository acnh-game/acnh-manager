#include "text_ui.hpp"

#include <cstdio>
#include <string>

#include "env/detect.hpp"
#include "install/engine.hpp"
#include "install/gate.hpp"
#include "log.hpp"
#include "version.hpp"

namespace acnh_manager::ui {
namespace {

#ifndef ACNH_BUILD_STAMP
#define ACNH_BUILD_STAMP "unknown"
#endif
constexpr const char *kBuildStamp = ACNH_BUILD_STAMP;
constexpr const char *kDevManifestPath = "/switch/ACNH-Manager/dev-manifest.json";
constexpr const char *kPayloadDir = "/switch/ACNH-Manager/payload";

const char *PlanName(install::PlanAction action) {
    switch (action) {
        case install::PlanAction::Blocked: return "BLOCKED";
        case install::PlanAction::Install: return "INSTALL";
        case install::PlanAction::Repair: return "REPAIR";
        case install::PlanAction::UpToDate: return "UP-TO-DATE";
    }
    return "?";
}

const char *GateName(install::GateStatus status) {
    switch (status) {
        case install::GateStatus::Supported: return "supported";
        case install::GateStatus::NoManifest: return "no manifest";
        case install::GateStatus::TitleNotSupported: return "title not supported";
        case install::GateStatus::VersionNotSupported: return "version not supported";
        case install::GateStatus::ContentIdMissing: return "content id unavailable";
        case install::GateStatus::ContentIdMismatch: return "content id mismatch";
        case install::GateStatus::BuildIdMismatch: return "running build id mismatch";
    }
    return "?";
}

}  // namespace

bool TextUi::Init() {
    consoleInit(nullptr);
    std::printf("ACNH-Manager %s (text UI)\n", acnh_manager::kAppVersion);
    consoleUpdate(nullptr);
    return true;
}

void TextUi::Exit() { consoleExit(nullptr); }

void TextUi::Run(acnh_manager::Log *log, FsFileSystem &sd) {
    bool dry_run = true;
    bool allow_dev_manifest = false;
    std::string status;

    auto refresh = [&](const char *note) {
        const auto report = env::Collect(sd);
        install::InstallState state{};
        bool has_state = false;
        std::string state_error;
        install::ReadStateFile(sd, &state, &has_state, &state_error);
        manifest::Manifest manifest{};
        bool has_manifest = false;
        std::string manifest_error;
        bool found = false;
        if (!install::ReadManifestFile(sd, kDevManifestPath, acnh_manager::kAppVersion, !allow_dev_manifest,
                                       &manifest, &found, &manifest_error)) {
            manifest_error = "manifest invalid: " + manifest_error;
        } else {
            has_manifest = found;
        }
        const auto gate = install::Evaluate(has_manifest ? &manifest : nullptr, report.build);
        const auto plan = install::Plan(gate, has_state ? &state : nullptr, manifest.agent);

        consoleClear();
        std::printf("ACNH-Manager %s (text UI)  build %s\n\n", acnh_manager::kAppVersion,
                    kBuildStamp);
        std::printf("system : HOS %s, applet=%d\n", report.hos_version.c_str(),
                    report.applet_type);
        std::printf("game   : running=%s  version=%u  contentId=%s\n",
                    report.application_running ? "yes" : "no", report.build.version,
                    report.build.content_id.empty() ? "(unavailable)"
                                                    : report.build.content_id.c_str());
        if (report.build.module_id_known) {
            std::printf("         running moduleId=%s\n", report.build.module_id.c_str());
        }
        std::printf("exefs  : %zu file(s)\n", report.exefs.size());
        for (const auto &file : report.exefs) {
            std::printf("         %-20s %llu B\n", file.name.c_str(),
                        static_cast<unsigned long long>(file.size));
        }
        if (report.legacy_cheat_present) {
            std::printf("legacy : cheat file present: %s\n", report.legacy_cheat_name.c_str());
        }
        std::printf("\nmanifest: %s\n", has_manifest ? "dev file loaded" : "not available");
        if (!manifest_error.empty()) {
            std::printf("          %s\n", manifest_error.c_str());
        }
        std::printf("gate   : %s\n", GateName(gate.status));
        std::printf("plan   : %s  (%s)\n", PlanName(plan.action), plan.reason.c_str());
        std::printf("mode   : dry-run=%s  allow-dev-manifest=%s\n", dry_run ? "ON" : "OFF",
                    allow_dev_manifest ? "ON" : "OFF");
        if (!status.empty()) {
            std::printf("\nlast   : %s\n", status.c_str());
        }
        if (!report.problems.empty()) {
            std::printf("\nwarn   : %s\n", report.problems.c_str());
        }
        std::printf("\n[A] install/repair   [X] uninstall   [Y] dry-run   [ZL] dev manifest"
                    "   [B] exit\n");
        if (note != nullptr) {
            std::printf("> %s\n", note);
        }
        consoleUpdate(nullptr);
    };

    refresh("ready");

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u32 down = padGetButtonsDown(&pad);
        if ((down & HidNpadButton_B) != 0 || (down & HidNpadButton_Plus) != 0) {
            break;
        }
        if ((down & HidNpadButton_Y) != 0) {
            dry_run = !dry_run;
            refresh(dry_run ? "dry-run enabled (verify only)" : "dry-run disabled (writes to SD)");
            continue;
        }
        if ((down & HidNpadButton_ZL) != 0) {
            allow_dev_manifest = !allow_dev_manifest;
            refresh(allow_dev_manifest ? "dev manifest allowed" : "dev manifest rejected");
            continue;
        }
        if ((down & HidNpadButton_X) != 0) {
            const auto result = install::Uninstall(sd, dry_run, nullptr);
            status = result.ok ? (std::string("uninstall ok, files=") +
                                  std::to_string(result.files_written) +
                                  (result.dry_run ? " (dry-run)" : ""))
                               : ("uninstall failed: " + result.error);
            if (log != nullptr) {
                log->Line("text-ui uninstall: ok=%d files=%d error=%s", result.ok ? 1 : 0,
                          result.files_written, result.error.c_str());
            }
            refresh(nullptr);
            continue;
        }
        if ((down & HidNpadButton_A) != 0) {
            std::string state_error;
            install::InstallState state{};
            bool has_state = false;
            install::ReadStateFile(sd, &state, &has_state, &state_error);
            manifest::Manifest manifest{};
            bool has_manifest = false;
            std::string manifest_error;
            bool found = false;
            if (!install::ReadManifestFile(sd, kDevManifestPath, acnh_manager::kAppVersion, !allow_dev_manifest,
                                           &manifest, &found, &manifest_error)) {
                status = "install refused: " + manifest_error;
                refresh(nullptr);
                continue;
            }
            has_manifest = found;
            const auto report = env::Collect(sd);
            const auto gate = install::Evaluate(has_manifest ? &manifest : nullptr, report.build);
            const auto plan = install::Plan(gate, has_state ? &state : nullptr, manifest.agent);
            if (plan.action == install::PlanAction::Blocked || plan.game == nullptr) {
                status = "install refused: " + plan.reason;
                refresh(nullptr);
                continue;
            }
            install::SdFolderPayloadSource source(sd, kPayloadDir);
            const auto result = install::Install(sd, manifest, *plan.game, source, dry_run, nullptr);
            status = result.ok ? (std::string("install ok, files=") +
                                  std::to_string(result.files_written) +
                                  (result.dry_run ? " (dry-run)" : ""))
                               : ("install failed: " + result.error);
            if (log != nullptr) {
                log->Line("text-ui install: ok=%d files=%d dry_run=%d error=%s", result.ok ? 1 : 0,
                          result.files_written, result.dry_run ? 1 : 0, result.error.c_str());
            }
            refresh(nullptr);
        }
    }
}

}  // namespace acnh_manager::ui
