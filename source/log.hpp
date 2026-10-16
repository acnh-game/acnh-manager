#pragma once

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "util/fs_path.hpp"

namespace acnh_manager {

/* Whether this run writes the log at all.

   Each line is one open -> write -> flush -> close per sink, and the file handle is deliberately
   not kept open (an open handle makes the card answer "Device or resource busy" to FTP, which is
   how this project reads log.txt while the app runs).  That discipline has a price: the lines of
   one startup, measured on hardware (2026-10-16), cost about 2.2 s -- most of the black screen
   before the first frame.  So writing it is a switch, and the switch is a *runtime* one:

     * a **development build always writes it** -- the probes, the timing lines and every
       diagnostic of the development loop exist nowhere else;
     * a **release build writes it only when the card carries `log-on`** (the file's existence is
       the whole protocol, like the other switch files), so a shipped binary can be debugged on a
       console without rebuilding it, and by default nobody pays 2.2 s for a log nobody reads.

   The decision is made once, at startup (see LogWanted), because the file handle discipline is
   per line: there is no "turn it on halfway". */
#ifndef ACNH_DEV_AIDS
#define ACNH_DEV_AIDS 0
#endif

/* The switch file: present = a release build writes its log this run.  Named without the `dev-`
   prefix the development switches use, because a *release* build honours this one. */
inline constexpr const char *kLogSwitchPath = "/switch/ACNH-Manager/log-on";

/* The decision itself, made once at startup (main calls it before opening the log): a development
   build always writes its log, a release build only when `log-on` sits on the card.  Only the
   existence of the file matters, never its contents -- the same protocol as the dev switches. */
inline bool LogWanted(FsFileSystem &sd) {
#if ACNH_DEV_AIDS
    (void)sd;
    return true;
#else
    FsFile flag{};
    const util::FsPath path(kLogSwitchPath);
    if (R_FAILED(fsFsOpenFile(&sd, path.c_str(), FsOpenMode_Read, &flag))) {
        return false;
    }
    fsFileClose(&flag);
    return true;
#endif
}

/* Line-based log file on the SD card.  Uses the fs* API directly, no devoptab mount.

   Three rules matter:
   1. **every line is opened -> written -> flushed -> closed**: that keeps the log readable
      over FTP while the app runs (an open handle makes fs answer "Device or resource
      busy") and loses nothing if we crash;
   2. fs cannot write past EOF, so the file is grown to the target length first (SetSize),
      exactly like Sphaira's write_entire_file;
   3. **every path handed to fs goes through util::FsPath first**: fs declares `FS_MAX_PATH`
      bytes for a path no matter how long it is, so the buffer has to be ours and cover that
      whole window (`source/util/fs_path.hpp`, `docs/architecture.md` section 4.2).  That is
      why the sink keeps its own path buffer instead of remembering the caller's pointer. */
class Log {
public:
    /* `truncate` = true: recreate the file (this run's log); false: append (kept across runs).
       `wanted` comes from LogWanted() and is passed in explicitly at every call site: a default
       here would let a release build start writing a log nobody asked for. */
    Result Open(FsFileSystem &sd, const char *path, bool truncate, bool wanted) {
        if (m_sink_count >= kMaxSinks || path == nullptr) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        if (!wanted) {
            /* Logging is off for this run (see LogWanted): not an error, but nothing is written and
               no file is created or deleted -- a log.txt another run left behind stays readable
               instead of being silently truncated to nothing. */
            return 0;
        }
        const util::FsPath arg(path);
        Sink &sink = m_sinks[m_sink_count];
        if (truncate) {
            fsFsDeleteFile(&sd, arg.c_str());
        }
        const Result rc = fsFsCreateFile(&sd, arg.c_str(), 0, 0);
        if (R_FAILED(rc) && rc != 0x00000402) { /* 402 = PathAlreadyExists */
            std::printf("log: create %s failed rc=0x%08X\n", arg.c_str(), rc);
            return rc;
        }
        s64 size = 0;
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file))) {
            fsFileGetSize(&file, &size);
            fsFileClose(&file);
        }
        sink.sd = &sd;
        std::snprintf(sink.path, sizeof(sink.path), "%s", arg.c_str());
        sink.offset = size;
        sink.open = true;
        ++m_sink_count;
        return 0;
    }

    ~Log() { Close(); }

    /* The handle is never kept open; this only marks the end. */
    void Close() {
        for (Sink &sink : m_sinks) {
            sink.open = false;
        }
    }

    /* The write path flushes and closes per line, so nothing to do here; kept for API
       symmetry. */
    void Sync() {}

    void Line(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if (n > static_cast<int>(sizeof(buf)) - 2) {
            n = static_cast<int>(sizeof(buf)) - 2;
        }
        buf[n++] = '\n';
        buf[n] = '\0';
        for (Sink &sink : m_sinks) {
            Write(sink, buf, static_cast<u64>(n));
        }
        if (m_echo) {
            std::printf("%s", buf);
        }
    }

    /* Turn console echo off (the report carries non-ASCII text and the console font is
       ASCII only). */
    void SetEcho(bool on) { m_echo = on; }

private:
    static constexpr size_t kMaxSinks = 2;

    struct Sink {
        FsFileSystem *sd{nullptr};
        /* Named, asserted, and big enough on purpose: this array is handed to fs* as a path, so
           it owes the whole `FS_MAX_PATH` window on its own (see rule 3 above).  It used to be
           128 bytes, which left the declared window running past the end of the array. */
        static constexpr size_t kPathBufferSize = util::FsPath::kBufferSize;
        static_assert(kPathBufferSize >= FS_MAX_PATH,
                      "the sink's path buffer must cover the whole FS_MAX_PATH IPC window");
        char path[kPathBufferSize]{};
        s64 offset{0};
        bool open{false};
        bool failed{false};
    };

    void Write(Sink &sink, const char *data, u64 size) {
        if (!sink.open || sink.sd == nullptr || sink.path[0] == '\0' || size == 0) {
            return;
        }
        FsFile file{};
        Result rc = fsFsOpenFile(sink.sd, sink.path, FsOpenMode_Write | FsOpenMode_Append, &file);
        if (R_FAILED(rc)) {
            if (!sink.failed) {
                sink.failed = true;
                std::printf("log: open %s failed rc=0x%08X\n", sink.path, rc);
            }
            return;
        }
        const s64 end = sink.offset + static_cast<s64>(size);
        rc = fsFileSetSize(&file, end);
        if (R_SUCCEEDED(rc)) {
            rc = fsFileWrite(&file, sink.offset, data, size, FsWriteOption_Flush);
        }
        if (R_SUCCEEDED(rc)) {
            rc = fsFileFlush(&file);
        }
        fsFileClose(&file);
        if (R_FAILED(rc)) {
            if (!sink.failed) {
                sink.failed = true;
                std::printf("log: write %s failed rc=0x%08X\n", sink.path, rc);
            }
            return;
        }
        sink.offset = end;
    }

    Sink m_sinks[kMaxSinks]{};
    size_t m_sink_count{0};
    bool m_echo{true};
};

}  // namespace acnh_manager
