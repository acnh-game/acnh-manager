/* Development aids only: with DEV_AIDS=0 this file compiles to nothing, like the probes. */
#ifndef ACNH_DEV_AIDS
#define ACNH_DEV_AIDS 0
#endif

#if ACNH_DEV_AIDS

#include "install/dev_stats.hpp"

#include <switch.h>

#include <cstdio>

#include "log.hpp"

namespace acnh_manager::install::dev {
namespace {

Stats g_stats;

void Add(Stat *stat, std::uint64_t begin_ms, std::size_t bytes) {
    const std::uint64_t now = NowMs();
    const std::uint64_t took = now > begin_ms ? now - begin_ms : 0;
    stat->count.fetch_add(1, std::memory_order_relaxed);
    stat->bytes.fetch_add(bytes, std::memory_order_relaxed);
    stat->ms.fetch_add(took, std::memory_order_relaxed);
    std::uint64_t seen = stat->max_ms.load(std::memory_order_relaxed);
    while (took > seen &&
           !stat->max_ms.compare_exchange_weak(seen, took, std::memory_order_relaxed)) {
    }
}

/* Milliseconds as seconds with one decimal: the log keeps these lines short and readable, and a
   breakdown nobody can read is not a breakdown. */
void Seconds(const char *label, std::uint64_t ms, char *out, std::size_t out_size) {
    std::snprintf(out, out_size, "%s %.1f s", label, static_cast<double>(ms) / 1000.0);
}

/* Bytes per second as MB/s with one decimal (decimal MB, the unit the screens use). */
double RateMb(std::uint64_t bytes, std::uint64_t ms) {
    if (ms == 0) {
        return 0.0;
    }
    return static_cast<double>(bytes) / 1000000.0 / (static_cast<double>(ms) / 1000.0);
}

}  // namespace

std::uint64_t NowMs() {
    /* The system tick is the only monotonic clock this process has (the time service reads the
       console's wall clock, which can jump); armTicksToNs turns it into nanoseconds. */
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

/* `Stats` holds atomics, so it cannot be assigned wholesale: clear the fields one by one. */
void Reset() {
    Stat *all[] = {&g_stats.source_read, &g_stats.decode, &g_stats.handout, &g_stats.placeholder,
                   &g_stats.writer_wait, &g_stats.reader_wait};
    for (Stat *stat : all) {
        stat->count.store(0);
        stat->bytes.store(0);
        stat->ms.store(0);
        stat->max_ms.store(0);
    }
    g_stats.transfer_ms.store(0);
    g_stats.steps.store(0);
}

void AddSourceRead(std::uint64_t begin_ms, std::size_t bytes) {
    Add(&g_stats.source_read, begin_ms, bytes);
}

void AddDecode(std::uint64_t begin_ms, std::size_t bytes) {
    Add(&g_stats.decode, begin_ms, bytes);
}

void AddHandout(std::uint64_t begin_ms, std::size_t bytes) {
    Add(&g_stats.handout, begin_ms, bytes);
}

void AddPlaceholder(std::uint64_t begin_ms, std::size_t bytes) {
    Add(&g_stats.placeholder, begin_ms, bytes);
}

void AddWriterWait(std::uint64_t begin_ms) {
    Add(&g_stats.writer_wait, begin_ms, 0);
}

void AddReaderWait(std::uint64_t begin_ms) {
    Add(&g_stats.reader_wait, begin_ms, 0);
}

void SetTransferMs(std::uint64_t ms) { g_stats.transfer_ms.store(ms); }

void AddStep() { g_stats.steps.fetch_add(1, std::memory_order_relaxed); }

void LogSummary(Log &log) {
    /* Read once into plain numbers: the counters are atomic and the two threads are still running
       while this is printed. */
    struct Plain {
        std::uint64_t count, bytes, ms, max_ms;
    };
    const auto plain = [](const Stat &s) {
        return Plain{s.count.load(), s.bytes.load(), s.ms.load(), s.max_ms.load()};
    };
    const Plain source_read = plain(g_stats.source_read);
    const Plain decode = plain(g_stats.decode);
    const Plain handout = plain(g_stats.handout);
    const Plain placeholder = plain(g_stats.placeholder);
    const std::uint64_t transfer_ms = g_stats.transfer_ms.load();
    const std::uint64_t steps = g_stats.steps.load();
    log.Line("timing: transfer %.1f s in %llu steps",
             static_cast<double>(transfer_ms) / 1000.0,
             static_cast<unsigned long long>(steps));
    log.Line("timing: source  n=%llu %llu bytes %.1f s (%.1f MB/s) mean %llu ms max %llu ms",
             static_cast<unsigned long long>(source_read.count),
             static_cast<unsigned long long>(source_read.bytes),
             static_cast<double>(source_read.ms) / 1000.0,
             RateMb(source_read.bytes, source_read.ms),
             static_cast<unsigned long long>(source_read.count ? source_read.ms / source_read.count
                                                               : 0),
             static_cast<unsigned long long>(source_read.max_ms));
    log.Line("timing: decode  n=%llu %llu bytes %.1f s (%.1f MB/s) mean %llu ms max %llu ms",
             static_cast<unsigned long long>(decode.count),
             static_cast<unsigned long long>(decode.bytes),
             static_cast<double>(decode.ms) / 1000.0,
             RateMb(decode.bytes, decode.ms),
             static_cast<unsigned long long>(decode.count ? decode.ms / decode.count : 0),
             static_cast<unsigned long long>(decode.max_ms));
    log.Line("timing: handout n=%llu %llu bytes %.1f s mean %llu ms max %llu ms",
             static_cast<unsigned long long>(handout.count),
             static_cast<unsigned long long>(handout.bytes),
             static_cast<double>(handout.ms) / 1000.0,
             static_cast<unsigned long long>(handout.count ? handout.ms / handout.count : 0),
             static_cast<unsigned long long>(handout.max_ms));
    log.Line("timing: write   n=%llu %llu bytes %.1f s (%.1f MB/s) mean %llu ms max %llu ms",
             static_cast<unsigned long long>(placeholder.count),
             static_cast<unsigned long long>(placeholder.bytes),
             static_cast<double>(placeholder.ms) / 1000.0,
             RateMb(placeholder.bytes, placeholder.ms),
             static_cast<unsigned long long>(placeholder.count ? placeholder.ms / placeholder.count
                                                               : 0),
             static_cast<unsigned long long>(placeholder.max_ms));
    char wait[64];
    Seconds("writer", g_stats.writer_wait.ms.load(), wait, sizeof(wait));
    char wait2[64];
    Seconds("reader", g_stats.reader_wait.ms.load(), wait2, sizeof(wait2));
    log.Line("timing: waits   %s, %s (the two halves of the pipeline)", wait, wait2);
}

std::uint64_t g_launch_ms = 0;

void MarkLaunch() {
    g_launch_ms = NowMs();
}

void LogStartup(Log &log, const char *phase) {
    const std::uint64_t now = NowMs();
    const std::uint64_t since =
        g_launch_ms != 0 && now > g_launch_ms ? now - g_launch_ms : 0;
    log.Line("launch: +%llu ms %s", static_cast<unsigned long long>(since), phase);
}

void LogPrepare(Log &log, std::uint64_t total_ms, std::uint64_t meta_ms, std::uint64_t control_ms,
                std::uint64_t query_ms) {
    log.Line("prepare: %llu ms total (meta reads %llu, control %llu, queries %llu)",
             static_cast<unsigned long long>(total_ms),
             static_cast<unsigned long long>(meta_ms),
             static_cast<unsigned long long>(control_ms),
             static_cast<unsigned long long>(query_ms));
}

}  // namespace acnh_manager::install::dev

#endif /* ACNH_DEV_AIDS */
