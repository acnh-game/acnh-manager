#pragma once

/* 真机环境采集(只读):把判据链需要的数字与"玩家会不会踩坑"的配置读出来。
   取数接口的选择依据见 docs/architecture.md 第 3、5 节(ns / ncm / dmnt:cht 可用;fsp-ldr 被拒)。 */

#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

#include "env/config_ini.hpp"
#include "install/gate.hpp"

namespace acnh_manager::env {

inline constexpr u64 kAcnhTitleId = 0x01006F8002326000ull;
inline constexpr u64 kAcnhUpdateTitleId = 0x01006F8002326800ull;

struct MetaEntry {
    u8 meta_type{0};
    u8 storage{0};
    std::uint32_t version{0};
    u64 application_id{0};
};

struct ExefsFile {
    std::string name;
    std::uint64_t size{0};
};

struct EnvironmentReport {
    /* 运行环境 */
    std::string hos_version; /* 形如 "22.1.0" */
    int applet_type{0};
    bool application_running{false};

    /* 游戏安装信息(判据 ① 与 ③ 的来源) */
    std::vector<MetaEntry> metas;
    bool patch_found{false};
    std::uint8_t patch_storage{0};
    install::DetectedBuild build;

    /* 覆盖配置(启动游戏时要不要按键) */
    bool have_override_config{false};
    bool have_title_config{false};
    OverrideAdvice advice;

    /* 安装现状 */
    std::vector<ExefsFile> exefs;
    bool legacy_cheat_present{false};
    std::string legacy_cheat_name;

    /* 失败诊断:为空表示所有取数都成功 */
    std::string problems;
};

/* 采集环境报告。只读:不写任何文件、不改任何配置。 */
EnvironmentReport Collect(FsFileSystem &sd);

/* 列出目录里的文件(名字 + 大小);目录不存在时返回空表。 */
std::vector<ExefsFile> ListDirectory(FsFileSystem &sd, const char *path);

}  // namespace acnh_manager::env
