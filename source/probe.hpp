#pragma once

#include <switch.h>

#include "log.hpp"

namespace acnh_manager::probe {

inline constexpr u64 kAcnhTitleId = 0x01006F8002326000ull;
inline constexpr u64 kAcnhUpdateTitleId = 0x01006F8002326800ull;

/* M0 环境探测:ns 版本、fsp-ldr code FS 各组合、可选 dmnt:cht 交叉校验、
   SD 上现有 exefs 覆盖状态。只读,不写任何游戏目录。 */
void Run(Log &log, FsFileSystem &sd);

}  // namespace acnh_manager::probe
