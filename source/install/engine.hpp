#pragma once

/* 安装 / 卸载引擎(设备侧)。纯逻辑部分(门控、状态模型、SHA-256、时间格式)在其它模块里,
   这里只负责"读文件、校验、落盘、记录"这几件必须碰 SD 卡的事。

   落盘约定(见 docs/architecture.md 第 4 节):先写临时文件 → 校验 → 再改名,
   且每次写入前必须 fsFileSetSize;绝不写半截文件。 */

#include <switch.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "install/gate.hpp"
#include "manifest/manifest.hpp"

namespace acnh_manager::install {

/* payload 来源:M2 从 SD 目录读(便于真机干跑),M4 换成 NRO 内嵌 romfs。 */
class PayloadSource {
public:
    virtual ~PayloadSource() = default;
    virtual bool Read(const std::string &name, std::vector<u8> *out, std::string *error) = 0;
};

class SdFolderPayloadSource final : public PayloadSource {
public:
    explicit SdFolderPayloadSource(FsFileSystem &sd,
                                   std::string dir = "/switch/ACNH-Manager/payload")
        : m_sd(&sd), m_dir(std::move(dir)) {}
    bool Read(const std::string &name, std::vector<u8> *out, std::string *error) override;

private:
    FsFileSystem *m_sd;
    std::string m_dir;
};

struct Progress {
    std::string step;
    int index{0};
    int total{0};
};

using ProgressCallback = std::function<void(const Progress &)>;

struct InstallResult {
    bool ok{false};
    std::string error;
    InstallState state;
    int files_written{0};
    bool dry_run{false};
};

/* 基础文件操作(公开给 UI 与后续里程碑复用)。 */
bool EnsureDirectory(FsFileSystem &sd, const std::string &path, std::string *error);
bool ReadWholeFile(FsFileSystem &sd, const std::string &path, std::vector<u8> *out,
                   std::string *error);
bool HashFile(FsFileSystem &sd, const std::string &path, std::string *hex, std::string *error);
/* 写文件并回读校验:临时文件 → SetSize → 写入 → 回读 sha256 → 原子改名。 */
bool WriteFileVerified(FsFileSystem &sd, const std::string &path, const std::vector<u8> &data,
                       const std::string &sha256, std::string *error);

bool ReadStateFile(FsFileSystem &sd, InstallState *state, bool *found, std::string *error);
bool WriteStateFile(FsFileSystem &sd, const InstallState &state, std::string *error);

/* 读取指定的清单文件(M2 阶段用 SD 上的开发用清单;M4 换成内嵌 romfs)。
   found=false 表示文件不存在(不是错误)。 */
bool ReadManifestFile(FsFileSystem &sd, const std::string &path, std::string_view app_version,
                      bool require_release_build, manifest::Manifest *out, bool *found,
                      std::string *error);

/* 完整安装。dry_run=true 时只做校验与回报,不写任何文件。 */
InstallResult Install(FsFileSystem &sd, const manifest::Manifest &manifest,
                      const manifest::GameEntry &game, PayloadSource &source, bool dry_run,
                      const ProgressCallback &progress);

/* 卸载:按 state.json 记录逐个校验 sha256 后删除,最后删记录与空目录。 */
InstallResult Uninstall(FsFileSystem &sd, bool dry_run, const ProgressCallback &progress);

}  // namespace acnh_manager::install
