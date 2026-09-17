#pragma once

/* 门控与安装决策:纯逻辑,无 libnx 依赖,主机侧可测。

   判据链(真机实测依据见 docs/architecture.md 第 3、5 节):
     ① ns  标题版本号(DetectedBuild::version)
     ② ncm 更新标题 Program 内容 id(DetectedBuild::content_id)
     ③ dmnt:cht 的 main ModuleId(DetectedBuild::module_id,仅游戏运行时可得)
   → 与清单 games[] 条目逐项比对;任何一环不符都 fail-closed,不做"最接近"匹配。 */

#include <cstdint>
#include <string>
#include <vector>

#include "manifest/manifest.hpp"

namespace acnh_manager::install {

struct DetectedBuild {
    std::string title_id;   /* 16 位十六进制 */
    std::uint32_t version{0};
    std::string content_id; /* 32 位十六进制;读不到时为空 */
    bool module_id_known{false};
    std::string module_id;  /* 32 位十六进制 */
};

enum class GateStatus {
    Supported,
    NoManifest,
    TitleNotSupported,
    VersionNotSupported,
    ContentIdMissing,
    ContentIdMismatch,
    BuildIdMismatch,
};

struct GateResult {
    GateStatus status{GateStatus::NoManifest};
    const manifest::GameEntry *game{nullptr};
    std::string reason; /* 诊断文本,UI 层据此组装面向玩家的说明 */
};

/* manifest 为 nullptr 表示 App 当前没有可用清单(M1 阶段即如此)。 */
GateResult Evaluate(const manifest::Manifest *manifest, const DetectedBuild &detected);

struct InstalledFile {
    std::string target;
    std::uint64_t size{0};
    std::string sha256;
};

/* state.json 的内存模型(/switch/ACNH-Manager/state.json)。 */
struct InstallState {
    std::string agent_version;
    std::string agent_commit;
    std::string content_id;
    std::string build_id;
    std::string installed_at;
    std::vector<InstalledFile> files;
};

enum class PlanAction {
    Blocked,  /* 门控不通过:不写任何文件 */
    Install,  /* 全新安装,或换构建/换 agent 版本 */
    Repair,   /* 记录存在但文件与清单不符 */
    UpToDate, /* 已是最新,可跳过写入 */
};

struct InstallPlan {
    PlanAction action{PlanAction::Blocked};
    const manifest::GameEntry *game{nullptr};
    std::string reason;
};

InstallPlan Plan(const GateResult &gate, const InstallState *state,
                 const manifest::AgentInfo &agent);

/* state.json 的读写(字符串形态,文件 IO 由调用方负责)。 */
std::string DumpState(const InstallState &state);
bool ParseState(std::string_view text, InstallState *out, std::string *error);

}  // namespace acnh_manager::install
