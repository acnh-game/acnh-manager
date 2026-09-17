#include "strings.hpp"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace acnh_manager::i18n {
namespace {

struct Entry {
    const char *zh;
    const char *en;
};

constexpr Entry kStrings[] = {
    {"ACNH-Manager", "ACNH-Manager"},
    {"状态", "Status"},
    {"设置", "Settings"},
    {"游戏", "Game"},
    {"覆盖设置", "Override"},
    {"安装现状", "Installed"},
    {"发布清单", "Manifest"},
    {"系统版本", "System"},
    {"运行模式", "Mode"},
    {"游戏是否运行", "Game running"},
    {"游戏版本号", "Game version"},
    {"更新包内容 id", "Update content id"},
    {"运行中构建号", "Running build id"},
    {"覆盖键提示", "Override key advice"},
    {"现有覆盖文件", "Existing override files"},
    {"旧金手指文件", "Legacy cheat file"},
    {"清单状态", "Manifest state"},
    {"门控结果", "Gate result"},
    {"取数异常", "Collection problems"},
    {"A 刷新   L/R 切换页面   B 退出", "A refresh   L/R switch page   B exit"},
    {"A 切换语言   L/R 切换页面   B 退出", "A toggle language   L/R switch page   B exit"},
    {"界面语言", "Language"},
    {"简体中文", "简体中文"},
    {"English", "English"},
    {"(不可用)", "(unavailable)"},
    {"(游戏未运行)", "(game not running)"},
    {"无", "none"},
    {"有", "present"},
    {"无", "absent"},
    {"未内置(等待发布导入)", "not embedded (pending release import)"},
    {"检测到旧金手指文件,与 agent 可能冲突,建议移入备份", "Legacy cheat file detected; it may conflict with the agent. Consider moving it to a backup."},
    {"按 A 重新采集环境", "Press A to re-collect the environment"},
    {"安装确认", "Install"},
    {"结果", "Result"},
    {"卸载", "Uninstall"},
    {"将要写入的文件", "Files to write"},
    {"payload 目录", "Payload folder"},
    {"干跑模式", "Dry run"},
    {"清单来源", "Manifest source"},
    {"开启(只校验不写盘)", "on (verify only)"},
    {"关闭(真正写入)", "off (write to SD)"},
    {"开发用清单文件", "dev manifest file"},
    {"A 开始   B 返回", "A run   B back"},
    {"A 重新采集   B 返回", "A re-collect   B back"},
    {"按下 A 开始安装", "Press A to install"},
    {"按下 A 卸载并回滚", "Press A to uninstall and roll back"},
    {"安装成功", "Install succeeded"},
    {"安装失败", "Install failed"},
    {"没有可用的发布清单:请放入开发用清单文件后重试", "No manifest available: add the dev manifest file and retry"},
    {"将删除 state.json 记录中的文件;被修改过的文件会被拒绝删除", "Files recorded in state.json will be removed; modified files are refused"},
    {"联网检查(+)", "Update check (+)"},
    {"(未检查)", "(not checked)"},
    {"清单可用,agent %s", "manifest ok, agent %s"},
    {"清单无效: %s", "invalid manifest: %s"},
    {"跳过: %s", "skipped: %s"},
    {"失败: %s", "failed: %s"},
    {"已内置(NRO 自带)", "embedded (bundled in the NRO)"},
    {"覆盖始终生效(override_key 没有有效按键)", "Override always applies (override_key has no valid key)"},
    {"覆盖永不生效(override_key 为空或按键名无法识别):请改成 !L 或删除该项", "Override never applies (override_key empty or the key name is unknown): change it to !L or remove the entry"},
    {"覆盖默认生效;启动游戏时不要按住 %s", "Override applies by default; do not hold %s while launching the game"},
    {"覆盖默认关闭;启动游戏时需要按住 %s", "Override is off by default; hold %s while launching the game"},
    {"。注意:按住 %s 启动任何应用会进入 hbmenu", ". Note: holding %s while launching any application opens hbmenu"},
    {"state.json 无法解析: %s", "state.json could not be parsed: %s"},
    {"内嵌清单无效: %s", "invalid embedded manifest: %s"},
    {"清单无效: %s", "invalid manifest: %s"},
    {"内嵌 payload 里没有文件 \"%s\"", "embedded payload has no file \"%s\""},
    {"payload 读取失败: %s", "payload read failed: %s"},
    {"payload %s 大小不符: %llu != %llu", "payload %s size mismatch: %llu != %llu"},
    {"payload %s sha256 不符: %s", "payload %s sha256 mismatch: %s"},
    {"写入失败: %s", "write failed: %s"},
    {"写入 state.json 失败: %s", "writing state.json failed: %s"},
    {"state.json 解析失败: %s", "state.json parse failed: %s"},
    {"没有安装记录,无需卸载", "nothing to uninstall (no install record)"},
    {"文件被修改过,拒绝删除: %s", "file was modified, refusing to delete: %s"},
    {"无法挂载 SD 卡", "cannot mount the SD card"},
    {"缺少 CA 文件: %s", "missing CA file: %s"},
    {"未配置 CA bundle", "no CA bundle configured"},
    {"只接受 https 地址", "only https URLs are accepted"},
    {"curl_easy_init 失败", "curl_easy_init failed"},
    {"socket 服务不可用(rc=0x%08X)", "socket service unavailable (rc=0x%08X)"},
    {"网络失败: %s", "network failed: %s"},
    {"已获取发布清单", "release manifest fetched"},
    {"按 B 退出", "Press B to exit"},
};

constexpr std::size_t kCount = sizeof(kStrings) / sizeof(kStrings[0]);

Language g_language = Language::ZhHans;

}  // namespace

const char *Text(StringId id, Language language) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kCount) {
        return "";
    }
    return language == Language::ZhHans ? kStrings[index].zh : kStrings[index].en;
}

unsigned StringCount() { return static_cast<unsigned>(kCount); }

std::string Format(StringId id, ...) {
    const char *format = Text(id, g_language);
    char stack_buffer[256];
    va_list args;
    va_start(args, id);
    const int needed = std::vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);
    if (needed < 0) {
        return format;
    }
    if (static_cast<std::size_t>(needed) < sizeof(stack_buffer)) {
        return stack_buffer;
    }
    std::vector<char> heap(static_cast<std::size_t>(needed) + 1);
    va_start(args, id);
    std::vsnprintf(heap.data(), heap.size(), format, args);
    va_end(args);
    return heap.data();
}

Language Current() { return g_language; }

void SetLanguage(Language language) { g_language = language; }

}  // namespace acnh_manager::i18n
