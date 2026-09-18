#pragma once

#include <switch.h>

#include "log.hpp"

namespace acnh_manager::probe {

inline constexpr u64 kAcnhTitleId = 0x01006F8002326000ull;
inline constexpr u64 kAcnhUpdateTitleId = 0x01006F8002326800ull;

/* M0 environment probes: ns version, fsp-ldr code FS combinations, optional dmnt:cht
   cross-check, and a read-only snapshot of the exefs override on the SD card. */
void Run(Log &log, FsFileSystem &sd);

/* SD write probe (development switch `/switch/ACNH-Manager/dev-writeprobe`): try
   create->write->read->delete on the same card for absolute vs relative paths and for a
   normal directory vs the game directory, logging every return code.  It exists to pin down
   "write failed: create ... rc=0x..." style problems.  Only runs when the switch is
   present, and it only creates and deletes its own temp files. */
void RunWriteProbe(Log &log, FsFileSystem &sd);

/* Touch probe (development switch `/switch/ACNH-Manager/dev-touchprobe`): activate the
   touchscreen, log whether hid's shared memory appeared, then report every touch for a few
   seconds.  It exists to prove on real hardware that touch works in applet mode and that
   HidTouchState.x/y really are screen coordinates before the UI depends on them. */
void RunTouchProbe(Log &log, FsFileSystem &sd);

/* fs-session probe (development switch `/switch/ACNH-Manager/dev-fsprobe`): run at startup and
   after every failed install/uninstall, to answer "can this process still use the card?" with
   evidence instead of guesswork: the same create is tried with a static buffer and with a heap
   buffer, in a known-good directory (`/switch`) and in the game directory, each buffer's
   `svcQueryMemory` region is dumped, and the installer's own write sequence is replayed step by
   step (`fsop:` lines, see `install::SetFsTraceSink`).  It only creates and deletes its own
   `fsprobe-*.tmp` files.  This is how the `0xD401` path-buffer bug was pinned down
   (`docs/device-acceptance.md`). */
void RunFsSessionProbe(Log &log, FsFileSystem &sd, const char *where);

}  // namespace acnh_manager::probe
