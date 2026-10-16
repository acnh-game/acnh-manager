#pragma once

/* Development-aid timing for the transfer path: where the wall clock of one install actually goes.

   This exists because the download runs at 4-9 MB/s while the same console moves 8.6 MB/s over
   plain FTP, and the same address timed 50.8 s in one run and 78.8 s in the next (2026-10-15,
   measured on hardware).  Wall clock alone cannot say whether that time sits in the network reads,
   the zstd decode, the placeholder writes, or one half of the pipeline waiting for the other, so
   each of them keeps its own counter here and the run ends with a breakdown in log.txt.

   Compiled out with the other development aids (`ACNH_DEV_AIDS=0`, which is what a release build
   uses): the call sites stay, the work and the strings disappear.  `NowMs` is then a constant --
   call sites may use it freely, they just do not pay for it. */

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef ACNH_DEV_AIDS
#define ACNH_DEV_AIDS 0
#endif

/* Two threads touch these counters now (the fetch thread reads the network, the writer thread
   decodes and writes), so they are atomic: a lost update would quietly make the breakdown lie. */
#include <atomic>

namespace acnh_manager {

class Log;

}

namespace acnh_manager::install::dev {

/* One measurement: how many calls, how many bytes, and the total/worst-case time they took. */
struct Stat {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> ms{0};
    std::atomic<std::uint64_t> max_ms{0};
};

struct Stats {
    Stat source_read; /* one ranged request: bytes coming off the network */
    Stat decode;      /* payload -> NCA bytes (zstd, or a copy for a stored-raw block) */
    Stat handout;     /* moving decoded bytes out of the decoder's buffer */
    Stat placeholder; /* writing one range into the content's placeholder */
    Stat writer_wait; /* the writer had nothing to write (the reader is behind) */
    Stat reader_wait; /* the reader had no free slot (the writer is behind) */
    std::atomic<std::uint64_t> transfer_ms{0};
    std::atomic<std::uint64_t> steps{0};
};

#if ACNH_DEV_AIDS

std::uint64_t NowMs();
/* Called by Begin() so a second run in one session does not inherit the first one's numbers. */
void Reset();
void AddSourceRead(std::uint64_t begin_ms, std::size_t bytes);
void AddDecode(std::uint64_t begin_ms, std::size_t bytes);
void AddHandout(std::uint64_t begin_ms, std::size_t bytes);
void AddPlaceholder(std::uint64_t begin_ms, std::size_t bytes);
void AddWriterWait(std::uint64_t begin_ms);
void AddReaderWait(std::uint64_t begin_ms);
void SetTransferMs(std::uint64_t ms);
void AddStep();
/* One short line per phase: the log truncates a long line, and a truncated breakdown is no
   breakdown. */
void LogSummary(Log &log);

#else

inline std::uint64_t NowMs() { return 0; }
inline void Reset() {}
inline void AddSourceRead(std::uint64_t, std::size_t) {}
inline void AddDecode(std::uint64_t, std::size_t) {}
inline void AddHandout(std::uint64_t, std::size_t) {}
inline void AddPlaceholder(std::uint64_t, std::size_t) {}
inline void AddWriterWait(std::uint64_t) {}
inline void AddReaderWait(std::uint64_t) {}
inline void SetTransferMs(std::uint64_t) {}
inline void AddStep() {}
inline void LogSummary(Log &) {}

#endif

}  // namespace acnh_manager::install::dev
