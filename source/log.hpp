#pragma once

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace acnh_manager {

/* SD 卡上的行式日志。直接用 fs* API,不依赖 devoptab 挂载。

   两个关键约定:
   1. **每写一行都 打开 → 写入 → flush → 关闭**:这样日志在 App 运行期间也能被 FTP 读取
      (保持打开时 fs 会返回 "Device or resource busy"),崩溃时也不会丢缓冲;
   2. fs 不允许写到 EOF 之外,写入前必须先把文件撑到目标长度(SetSize),
      顺序与 Sphaira 的 write_entire_file 一致。 */
class Log {
public:
    /* truncate = true:重建文件(每次运行的当前日志);false:追加(跨运行保留)。 */
    Result Open(FsFileSystem &sd, const char *path, bool truncate) {
        if (m_sink_count >= kMaxSinks || path == nullptr) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        Sink &sink = m_sinks[m_sink_count];
        if (truncate) {
            fsFsDeleteFile(&sd, path);
        }
        const Result rc = fsFsCreateFile(&sd, path, 0, 0);
        if (R_FAILED(rc) && rc != 0x00000402) { /* 402 = PathAlreadyExists */
            std::printf("log: create %s failed rc=0x%08X\n", path, rc);
            return rc;
        }
        s64 size = 0;
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&sd, path, FsOpenMode_Read, &file))) {
            fsFileGetSize(&file, &size);
            fsFileClose(&file);
        }
        sink.sd = &sd;
        std::snprintf(sink.path, sizeof(sink.path), "%s", path);
        sink.offset = size;
        sink.open = true;
        ++m_sink_count;
        return 0;
    }

    ~Log() { Close(); }

    /* 句柄不保持打开,这里只标记结束。 */
    void Close() {
        for (Sink &sink : m_sinks) {
            sink.open = false;
        }
    }

    /* 写入路径逐行 flush+close,无需额外动作;保留此接口给调用方语义用。 */
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

    /* 关掉控制台回显(报告含中文,控制台只有 ASCII 字体会变乱码)。 */
    void SetEcho(bool on) { m_echo = on; }

private:
    static constexpr size_t kMaxSinks = 2;

    struct Sink {
        FsFileSystem *sd{nullptr};
        char path[128]{};
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
