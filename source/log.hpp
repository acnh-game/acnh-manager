#pragma once

#include <switch.h>

#include <cstdarg>
#include <cstdio>

namespace acnh_manager {

/* SD 卡上的行式日志:每次运行重建文件,写入位置单调递增。
   直接用 fs* API,不依赖 devoptab 挂载。

   注意:fs 不允许写到 EOF 之外,每次写入前必须先把文件撑到目标长度
   (Sphaira 的 write_entire_file 也是 CreateFile -> Open -> SetSize -> Write 这个顺序)。 */
class Log {
public:
    /* truncate = true:重建文件(每次运行的当前日志)。
       truncate = false:追加(跨运行保留的历史日志)。 */
    Result Open(FsFileSystem &sd, const char *path, bool truncate) {
        if (m_sink_count >= 2) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        Sink &sink = m_sinks[m_sink_count];
        Result rc;
        if (truncate) {
            fsFsDeleteFile(&sd, path);
            rc = fsFsCreateFile(&sd, path, 0, 0);
            if (R_FAILED(rc)) {
                std::printf("log: create %s failed rc=0x%08X\n", path, rc);
                return rc;
            }
            rc = fsFsOpenFile(&sd, path, FsOpenMode_Write, &sink.file);
        } else {
            rc = fsFsOpenFile(&sd, path, FsOpenMode_Write | FsOpenMode_Append, &sink.file);
            if (R_FAILED(rc)) {
                rc = fsFsCreateFile(&sd, path, 0, 0);
                if (R_SUCCEEDED(rc)) {
                    rc = fsFsOpenFile(&sd, path, FsOpenMode_Write | FsOpenMode_Append, &sink.file);
                }
            }
        }
        if (R_FAILED(rc)) {
            std::printf("log: open %s failed rc=0x%08X\n", path, rc);
            return rc;
        }
        s64 size = 0;
        sink.offset = R_SUCCEEDED(fsFileGetSize(&sink.file, &size)) ? size : 0;
        sink.open = true;
        ++m_sink_count;
        return 0;
    }

    ~Log() { Close(); }

    void Close() {
        for (Sink &sink : m_sinks) {
            if (sink.open) {
                fsFileFlush(&sink.file);
                fsFileClose(&sink.file);
                sink.open = false;
            }
        }
    }

    /* 把每个日志文件当前长度打到控制台,确认写入真的落盘。 */
    void Sync() {
        for (Sink &sink : m_sinks) {
            if (!sink.open) {
                continue;
            }
            fsFileFlush(&sink.file);
            s64 size = 0;
            if (R_SUCCEEDED(fsFileGetSize(&sink.file, &size))) {
                std::printf("log: on-disk size=%lld\n", static_cast<long long>(size));
            }
        }
    }

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

    /* 关掉控制台回显:报告里含中文,而控制台只有 ASCII 字体,回显会变乱码。 */
    void SetEcho(bool on) { m_echo = on; }

private:
    struct Sink {
        FsFile file{};
        s64 offset{0};
        bool open{false};
        bool failed{false};
    };

    void Write(Sink &sink, const char *data, u64 size) {
        if (!sink.open || size == 0) {
            return;
        }
        const s64 end = sink.offset + static_cast<s64>(size);
        Result rc = fsFileSetSize(&sink.file, end);
        if (R_SUCCEEDED(rc)) {
            rc = fsFileWrite(&sink.file, sink.offset, data, size, FsWriteOption_Flush);
        }
        if (R_FAILED(rc)) {
            if (!sink.failed) {
                sink.failed = true;
                std::printf("log: write failed rc=0x%08X\n", rc);
            }
            return;
        }
        sink.offset = end;
    }

    Sink m_sinks[2]{};
    size_t m_sink_count{0};
    bool m_echo{true};
};

}  // namespace acnh_manager
