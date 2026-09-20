#pragma once

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "util/fs_path.hpp"

namespace acnh_manager {

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
    /* truncate = true: recreate the file (this run's log); false: append (kept across runs). */
    Result Open(FsFileSystem &sd, const char *path, bool truncate) {
        if (m_sink_count >= kMaxSinks || path == nullptr) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
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
