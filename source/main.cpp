#include <switch.h>

#include "log.hpp"
#include "probe.hpp"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);
    std::printf("ACNH-Manager M0 spike\nlog: /switch/ACNH-Manager/spike.log\n");
    consoleUpdate(nullptr);

    const Result rc_fs = fsInitialize();
    FsFileSystem sd{};
    const Result rc_sd = R_SUCCEEDED(rc_fs) ? fsOpenSdCardFileSystem(&sd) : rc_fs;
    Result rc_dir = rc_sd;
    Result rc_log = rc_sd;
    acnh_manager::Log log;
    if (R_SUCCEEDED(rc_sd)) {
        rc_dir = fsFsCreateDirectory(&sd, "/switch/ACNH-Manager");
        rc_log = log.Open(sd, "/switch/ACNH-Manager/spike.log", true);
        if (R_SUCCEEDED(rc_log)) {
            log.Open(sd, "/switch/ACNH-Manager/spike-history.log", false);
        }
    }
    if (R_SUCCEEDED(rc_log)) {
        log.Line("m0: fsInitialize rc=0x%08X openSdmc rc=0x%08X mkdir rc=0x%08X", rc_fs, rc_sd, rc_dir);
        log.Sync();
        acnh_manager::probe::Run(log, sd);
        log.Line("=== done: press + to exit ===");
        log.Close();
    } else {
        std::printf("fatal: cannot open spike log (sdmc rc=0x%08X log rc=0x%08X)\n", rc_sd, rc_log);
        consoleUpdate(nullptr);
    }

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
    consoleExit(nullptr);
    return 0;
}
