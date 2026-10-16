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

/* Large-file probe (development switch `/switch/ACNH-Manager/dev-largefileprobe`): answer "how
   does this card handle a file larger than 4 GiB?" from the console's own side, in two halves,
   because the two ways of growing a file can differ:

     A. create a file and *tell* the filesystem how big it will be (`fsFileSetSize`, 4.5 GB);
     B. create a file and grow it by writing, never asking for a size up front.

   Then it reads the size and the last bytes back and deletes the file.  The question comes from
   watching a 4,650,239,771-byte package land in the card's root through DBI's FTP server while
   this project's own upload tools (sys-agent's FTP, sphaira's file transfer) both failed on it
   -- and both of those call `fsFileSetSize` with a size above 4 GiB.  It writes one file of
   4.5 GB in the manager's own directory and removes it; nothing else on the card is touched. */
void RunLargeFileProbe(Log &log, FsFileSystem &sd);

/* Network probe (development switch `/switch/ACNH-Manager/dev-netprobe`): measure what limits the
   download path, in the shapes the installer could use, with the same libcurl the installer uses
   and over the same address:

     A. one request, no Range: the whole body streamed into a counter ("not chunked at all");
     B. the installer's own shape: sequential 4 MiB Range requests;
     C. four connections, each walking its own slice with 4 MiB Range requests (parallel ranges);
     D. A again over a plain-http address, when a second line names one: the price of TLS;
     E. ten 1-byte Range requests: the per-request overhead on its own.

   The shapes exist because wall-clock times of whole installs (50.8 s and 78.8 s for the same
   address, same console, same package) cannot say whether the limit is the network, the TLS
   stack, the per-request cost, or something below all three.  Addresses come from
   `/switch/ACNH-Manager/dev-netprobe-url` (line 1 = the address to test, line 2 = optional
   plain-http address); every shape moves up to 64 MiB and logs bytes, seconds and MB/s. */
void RunNetworkProbe(Log &log, FsFileSystem &sd);

}  // namespace acnh_manager::probe
