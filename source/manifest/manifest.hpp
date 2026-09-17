#pragma once

/* 发布清单(agent-manifest.json)的解析与校验。
   纯 C++17、无 libnx 依赖,主机侧可直接测试。

   契约要点(完整说明见 ../../../docs/acnh_manager_plan.md 第 6 节):
   - schema 必须等于 kSchemaVersion;
   - agent.dirty 必须为 false、agent.buildFlags 必须等于 kReleaseBuildFlags
     (只带语义钩子位),否则视为"非发布产物",拒绝使用;
   - 每个文件:target 必须是相对 sdmc:/ 的安全路径、size > 0、sha256 为 64 位十六进制;
   - strict 模式下(默认)任何一项不符即整体拒绝,不做"跳过坏条目继续"。 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acnh_manager::manifest {

inline constexpr int kSchemaVersion = 1;
/* 语义钩子位(bit1);发布构建不得带 DEV / RPC server / 其他实验位。 */
inline constexpr std::uint32_t kReleaseBuildFlags = 1u << 1;

struct FileEntry {
    std::string name;
    std::string source;   /* 相对 baseUrl 的文件名 */
    std::string target;   /* 相对 sdmc:/ 的路径 */
    std::uint64_t size{0};
    std::string sha256;   /* 64 位十六进制 */
    std::string restart;  /* none | game | console */
};

struct GameEntry {
    std::string profile;
    std::string title_id;        /* 16 位十六进制,如 01006F8002326000 */
    std::uint32_t version{0};    /* 标题版本号,如 2228224 */
    std::string display_version; /* 人读版本,如 3.0.3 */
    std::string content_id;      /* 更新标题 Program NCA 的内容 id(32 位十六进制) */
    std::string build_id;        /* main 模块 ModuleId(32 位十六进制),可选但推荐 */
    std::vector<FileEntry> files;
};

struct AgentInfo {
    std::string version;
    std::string commit;
    std::uint32_t build_flags{0};
    bool dirty{true};
};

struct Manifest {
    int schema{0};
    std::string channel;
    std::string generated;
    std::string base_url;
    std::string changelog;
    std::string app_min_version;
    AgentInfo agent;
    std::vector<GameEntry> games;
};

struct ParseResult {
    bool ok{false};
    std::string error;
    Manifest manifest;
};

/* app_version 用点分十进制(如 "0.1.0");用于校验 app.minVersion。 */
ParseResult Parse(std::string_view text, std::string_view app_version);

/* target 安全检查:非空、无前导 '/'、无 '\\'、无 ".." 片段。 */
bool IsSafeTarget(std::string_view target);
bool IsHex(std::string_view text, std::size_t length);
bool IsSha256Hex(std::string_view text);

/* 版本比较:返回 <0 / 0 / >0(按点分十进制逐段比较,缺失段视为 0)。 */
int CompareVersions(std::string_view lhs, std::string_view rhs);

}  // namespace acnh_manager::manifest
