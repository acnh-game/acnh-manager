#pragma once

/* 联网检查更新(HTTPS 拉取发布清单)。

   设计原则:
   - 只做"检查",绝不在失败时影响安装决策:拿不到就退回内置/本地清单,并记录原因;
   - 证书校验是硬要求:必须提供 CA bundle(默认 `/switch/ACNH-Manager/ca.pem`,
     也可放在 romfs 由 M4 内嵌)。缺 CA 时直接跳过并说明,不做"关闭校验"的降级;
   - 超时短(连接 5s、总 8s),失败静默。 */

#include <string>

namespace acnh_manager::net {

struct UpdateCheckResult {
    bool attempted{false};
    bool ok{false};
    bool skipped{false};
    long http_code{0};
    std::string reason;        /* 诊断文本(给日志与设置页) */
    std::string manifest_text; /* 成功时的清单正文 */
};

UpdateCheckResult CheckForUpdate(const std::string &url, const std::string &ca_path,
                                 long timeout_seconds = 8);

/* 默认发布清单地址(指南站托管,与商店包共用同一份)。 */
inline constexpr const char *kDefaultManifestUrl =
    "https://lextuo.com/acnh-chat-code/guide/agent-manifest.json";
inline constexpr const char *kDefaultCaPath = "/switch/ACNH-Manager/ca.pem";

}  // namespace acnh_manager::net
